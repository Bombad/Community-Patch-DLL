﻿/*	-------------------------------------------------------------------------------------------------------
	� 1991-2012 Take-Two Interactive Software and its subsidiaries.  Developed by Firaxis Games.  
	Sid Meier's Civilization V, Civ, Civilization, 2K Games, Firaxis Games, Take-Two Interactive Software 
	and their respective logos are all trademarks of Take-Two interactive Software, Inc.  
	All other marks and trademarks are the property of their respective owners.  
	All rights reserved. 
	------------------------------------------------------------------------------------------------------- */

//
//  FILE:    CvAStar.cpp
//
//  AUTHOR:  Casey O'Toole  --  8/27/2002
//  MOD:     Brian Wade     --  5/20/2008
//  MOD:     Ed Beach       --  4/16/2009 moved into CvGameCoreDLL
//

#include "CvGameCoreDLLPCH.h"
#include "CvGameCoreUtils.h"
#include "CvAStar.h"
#include "ICvDLLUserInterface.h"
#include "CvMinorCivAI.h"
#include "CvDllInterfaces.h"
#include "cvStopWatch.h"
#include "CvUnitMovement.h"

#define PATH_MOVEMENT_WEIGHT									(100)	//a normal move costs 60 movement points!
#define PATH_MOVEMENT_BURN_WEIGHT								(50)	//leftover movement points at the target are mostly wasted
#define PATH_RIVER_WEIGHT										(100)	//per percent river crossing penalty on attack
#define PATH_DEFENSE_WEIGHT										(10)	//per percent defense bonus on turn end plot
#define PATH_TERRITORY_WEIGHT									(50)	//per turn end plot outside of our territory
#define PATH_STEP_WEIGHT										(50)	//per plot in path
#define PATH_THROUGH_WATER										(PATH_MOVEMENT_WEIGHT*60)
#define	PATH_EXPLORE_NON_HILL_WEIGHT							(1000)	//per hill plot we fail to visit
#define PATH_EXPLORE_NON_REVEAL_WEIGHT							(1000)	//per (neighboring) plot we fail to reveal
#define PATH_BUILD_ROUTE_BASE_VALUE								(1000)
#define PATH_BUILD_ROUTE_REUSE_EXISTING_WEIGHT					(10)
#define PATH_BUILD_ROUTE_ALREADY_FLAGGED_DISCOUNT				(0.5f)
#define PATH_TRADE_BASE_COST									(100)
#define PATH_END_TURN_LOW_DANGER_WEIGHT							(PATH_MOVEMENT_WEIGHT*40)
#define PATH_END_TURN_HIGH_DANGER_WEIGHT						(PATH_MOVEMENT_WEIGHT*90)
#define PATH_END_TURN_MORTAL_DANGER_WEIGHT						(PATH_MOVEMENT_WEIGHT*210)	//one of these is worth 3.5 plots of detour
#define PATH_END_TURN_MISSIONARY_OTHER_TERRITORY				(PATH_MOVEMENT_WEIGHT*210)	//don't make it even so we don't get ties
#define PATH_DO_NOT_USE_WEIGHT									(1000000000)

#include <xmmintrin.h>
#include "LintFree.h"

#define PREFETCH_FASTAR_NODE(x) _mm_prefetch((const char*)x,  _MM_HINT_T0 ); _mm_prefetch(((const char*)x)+64,  _MM_HINT_T0 );
#define PREFETCH_FASTAR_CVPLOT(x) _mm_prefetch((const char*)x,  _MM_HINT_T0 ); _mm_prefetch(((const char*)x)+64,  _MM_HINT_T0 );

//	--------------------------------------------------------------------------------
/// Constructor
CvAStar::CvAStar()
{
	udIsPathDest = NULL;
	udDestValid = NULL;
	udHeuristic = NULL;
	udCost = NULL;
	udValid = NULL;
	udNotifyChild = NULL;
	udNotifyList = NULL;
	udNumExtraChildrenFunc = NULL;
	udGetExtraChildFunc = NULL;
	udInitializeFunc = NULL;
	udUninitializeFunc = NULL;

	m_pOpen = NULL;
	m_pOpenTail = NULL;
	m_pClosed = NULL;
	m_pBest = NULL;
	m_pStackHead = NULL;

	m_ppaaNodes = NULL;
	m_ppaaNeighbors = NULL;

	m_iCurrentGenerationID = 0;

#if defined(MOD_BALANCE_CORE)
	//for debugging
	m_strName = "AStar";
#endif

	//this matches the default setting for SPathFinderUserData
	SetFunctionPointers(PathDest, StepDestValid, StepHeuristic, StepCost, StepValidAnyArea, StepAdd, NULL, NULL, NULL, NULL, NULL);
}

//	--------------------------------------------------------------------------------
/// Destructor
CvAStar::~CvAStar()
{
	DeInit();
}

//	--------------------------------------------------------------------------------
/// Frees allocated memory
void CvAStar::DeInit()
{
	if(m_ppaaNodes != NULL)
	{
		for(int iI = 0; iI < m_iColumns; iI++)
		{
			FFREEALIGNED(m_ppaaNodes[iI]);
		}

		FFREEALIGNED(m_ppaaNodes);
		m_ppaaNodes=0;
	}

	if (m_ppaaNeighbors)
	{
		delete [] m_ppaaNeighbors;
		m_ppaaNeighbors = NULL;
	}
}

//	--------------------------------------------------------------------------------
/// Initializes the AStar algorithm
void CvAStar::Initialize(int iColumns, int iRows, bool bWrapX, bool bWrapY)
{
	DeInit();	// free old memory just in case

	m_iColumns = iColumns;
	m_iRows = iRows;

	m_iXstart = -1;
	m_iYstart = -1;
	m_iXdest = -1;
	m_iYdest = -1;

	m_bWrapX = bWrapX;
	m_bWrapY = bWrapY;

	m_pOpen = NULL;
	m_pOpenTail = NULL;
	m_pClosed = NULL;
	m_pBest = NULL;
	m_pStackHead = NULL;

	m_iTestedNodes = 0;
	m_iProcessedNodes = 0;
	m_iBasicPlotCost = 1;

	m_ppaaNodes = reinterpret_cast<CvAStarNode**>(FMALLOCALIGNED(sizeof(CvAStarNode*)*m_iColumns, 64, c_eCiv5GameplayDLL, 0));
	for(int iI = 0; iI < m_iColumns; iI++)
	{
		m_ppaaNodes[iI] = reinterpret_cast<CvAStarNode*>(FMALLOCALIGNED(sizeof(CvAStarNode)*m_iRows, 64, c_eCiv5GameplayDLL, 0));
		for(int iJ = 0; iJ < m_iRows; iJ++)
		{
			new(&m_ppaaNodes[iI][iJ]) CvAStarNode();
			m_ppaaNodes[iI][iJ].m_iX = iI;
			m_ppaaNodes[iI][iJ].m_iY = iJ;
		}
	}

	m_ppaaNeighbors = new CvAStarNode*[m_iColumns*m_iRows*6];
	CvAStarNode** apNeighbors = m_ppaaNeighbors;

	for(int iI = 0; iI < m_iColumns; iI++)
	{
		for(int iJ = 0; iJ < m_iRows; iJ++)
		{
			m_ppaaNodes[iI][iJ].m_apNeighbors = apNeighbors;
			apNeighbors += 6;
			PrecalcNeighbors( &(m_ppaaNodes[iI][iJ]) );
		}
	}
}

void CvAStar::SetFunctionPointers(CvAPointFunc IsPathDestFunc, CvAPointFunc DestValidFunc, CvAHeuristic HeuristicFunc, 
						CvAStarConst1Func CostFunc, CvAStarConst2Func ValidFunc, CvAStarFunc NotifyChildFunc, CvAStarFunc NotifyListFunc, 
						CvANumExtraChildren NumExtraChildrenFunc, CvAGetExtraChild GetExtraChildFunc, CvABegin InitializeFunc, CvAEnd UninitializeFunc)
{
	udIsPathDest = IsPathDestFunc;
	udDestValid = DestValidFunc;
	udHeuristic = HeuristicFunc;
	udCost = CostFunc;
	udValid = ValidFunc;
	udNotifyChild = NotifyChildFunc;
	udNotifyList = NotifyListFunc;
	udNumExtraChildrenFunc = NumExtraChildrenFunc;
	udGetExtraChildFunc = GetExtraChildFunc;
	udInitializeFunc = InitializeFunc;
	udUninitializeFunc = UninitializeFunc;
}

//	--------------------------------------------------------------------------------
/// Generates a path from iXstart,iYstart to iXdest,iYdest
bool CvAStar::GeneratePathWithCurrentConfiguration(int iXstart, int iYstart, int iXdest, int iYdest, const SPathFinderUserData& data)
{
	if (data.ePathType != m_sData.ePathType)
		return false;

	CvAStarNode* temp;

	//this is the version number for the node cache
	m_iCurrentGenerationID++;
	if (m_iCurrentGenerationID==0xFFFF)
		m_iCurrentGenerationID = 1;

	m_sData = data;
	m_iXdest = iXdest;
	m_iYdest = iYdest;
	m_iXstart = iXstart;
	m_iYstart = iYstart;

	m_iProcessedNodes = 0;
	m_iTestedNodes = 0;

	if (udInitializeFunc)
		udInitializeFunc(m_sData, this);

	if(!isValid(iXstart, iYstart))
	{
		if (udUninitializeFunc)
			udUninitializeFunc(m_sData, this);
		return false;
	}

	PREFETCH_FASTAR_NODE(&(m_ppaaNodes[iXdest][iYdest]));

	//reset previously used nodes
	if(m_pOpen)
	{
		while(m_pOpen)
		{
			temp = m_pOpen->m_pNext;
			m_pOpen->clear();
			m_pOpen = temp;
		}
	}

	if(m_pClosed)
	{
		while(m_pClosed)
		{
			temp = m_pClosed->m_pNext;
			m_pClosed->clear();
			m_pClosed = temp;
		}
	}

	PREFETCH_FASTAR_NODE(&(m_ppaaNodes[iXstart][iYstart]));

	m_pBest = NULL;
	m_pStackHead = NULL;

	//set up first node
	temp = &(m_ppaaNodes[iXstart][iYstart]);
	temp->m_iKnownCost = 0;
	if(udHeuristic == NULL)
	{
		temp->m_iHeuristicCost = 0;
	}
	else
	{
		temp->m_iHeuristicCost = udHeuristic(m_iXstart, m_iYstart, m_iXdest, m_iYdest);
	}
	temp->m_iTotalCost = temp->m_iKnownCost + temp->m_iHeuristicCost;

	m_pOpen = temp;
	m_pOpenTail = temp;
	udFunc(udNotifyList, NULL, m_pOpen, ASNL_STARTOPEN, m_sData);
	udFunc(udValid, NULL, temp, 0, m_sData);
	udFunc(udNotifyChild, NULL, temp, ASNC_INITIALADD, m_sData);

	if(udDestValid != NULL)
	{
		if(!udDestValid(iXdest, iYdest, m_sData, this))
		{
			if (udUninitializeFunc)
				udUninitializeFunc(m_sData, this);
			return false;
		}
	}

	if(isValid(m_iXdest, m_iYdest))
	{
		temp = &(m_ppaaNodes[m_iXdest][m_iYdest]);

		if(temp->m_eCvAStarListType == CVASTARLIST_CLOSED)
		{
			m_pBest = temp;
			if (udUninitializeFunc)
				udUninitializeFunc(m_sData, this);
			return true;
		}
	}

#if defined(MOD_BALANCE_CORE_DEBUGGING)
	cvStopWatch timer("pathfinder");
	timer.StartPerfTest();
#endif

	//here the magic happens
	bool bSuccess = false;
	while(1)
	{
		m_pBest = GetBest();
		
		if (m_pBest==NULL)
			break;

		if(IsPathDest(m_pBest->m_iX, m_pBest->m_iY))
		{
			bSuccess = true;
			break;
		}

		CreateChildren(m_pBest);
	}

#if defined(MOD_BALANCE_CORE_DEBUGGING)
	//debugging!
	timer.EndPerfTest();

	if (m_iProcessedNodes>1000)
	{
		//in some cases we have no destination plot, so exhaustion is not always a "fail"
		OutputDebugString( CvString::format("Path type %d %s, tested %d nodes, processed %d nodes in %.2f ms (path length %d)\n", 
			m_sData.ePathType, bSuccess?"found":"not found", m_iTestedNodes, m_iProcessedNodes, timer.GetDeltaInSeconds()*1000, bSuccess?GetPathLength():-1 ).c_str() );

		if (false) //bSuccess
		{
			CvString fname = CvString::format( "PathfindingTurn%03d.txt", GC.getGame().getGameTurn() );
			FILogFile* pLog=LOGFILEMGR.GetLog( fname.c_str(), FILogFile::kDontTimeStamp );
			if (pLog) 
			{
				if (m_sData.iUnitID>0)
				{
					CvUnit* pUnit = GET_PLAYER(m_sData.ePlayer).getUnit(m_sData.iUnitID); 
					pLog->Msg( CvString::format("%s for %s (%d) from %d,%d to %d,%d for player %d\n", 
						GetName(),pUnit->getName().c_str(),pUnit->GetID(),m_iXstart,m_iYstart,m_iXdest,m_iYdest,pUnit->getOwner() ).c_str() );
				}

				gStackWalker.SetLog(pLog);
				gStackWalker.ShowCallstack();

				CvAStarNode* pCurrent = m_pBest;
				while (pCurrent!=NULL)
				{
					pLog->Msg( CvString::format("%d,%d cost %d,%d\n", pCurrent->m_iX,pCurrent->m_iY,pCurrent->m_iKnownCost,pCurrent->m_iHeuristicCost ).c_str() );
					pCurrent = pCurrent->m_pParent;
				}
			}
		}
	}
#endif

	if (udUninitializeFunc)
		udUninitializeFunc(m_sData, this);

	return bSuccess;
}

//	--------------------------------------------------------------------------------
/// Returns best node
CvAStarNode* CvAStar::GetBest()
{
	CvAStarNode* temp;
	if(!m_pOpen)
	{
		return NULL;
	}

	temp = m_pOpen;

	m_pOpen = temp->m_pNext;
	if(m_pOpen != NULL)
	{
		m_pOpen->m_pPrev = NULL;
	}
	else
	{
		m_pOpenTail = NULL;
	}

	udFunc(udNotifyList, NULL, temp, ASNL_DELETEOPEN, m_sData);

	temp->m_eCvAStarListType = CVASTARLIST_CLOSED;

	temp->m_pNext = m_pClosed;
	if(m_pClosed != NULL)
	{
		m_pClosed->m_pPrev = temp;
	}
	m_pClosed = temp;

	udFunc(udNotifyList, NULL, m_pClosed, ASNL_ADDCLOSED, m_sData);

	return temp;
}

// --------------------
/// precompute neighbors for a node
void CvAStar::PrecalcNeighbors(CvAStarNode* node)
{
	int range = 6;
	int x, y;

	static int s_CvAStarChildHexX[6] = { 0, 1,  1,  0, -1, -1, };
	static int s_CvAStarChildHexY[6] = { 1, 0, -1, -1,  0,  1, };

	for(int i = 0; i < range; i++)
	{
		x = node->m_iX - ((node->m_iY >= 0) ? (node->m_iY>>1) : ((node->m_iY - 1)/2));
		x += s_CvAStarChildHexX[i];
		y = yRange(node->m_iY + s_CvAStarChildHexY[i]);
		x += ((y >= 0) ? (y>>1) : ((y - 1)/2));
		x = xRange(x);
		y = yRange(y);

		if(isValid(x, y))
			node->m_apNeighbors[i] = &(m_ppaaNodes[x][y]);
		else
			node->m_apNeighbors[i] = NULL;
	}
}

//	--------------------------------------------------------------------------------
/// Creates children for the node
void CvAStar::CreateChildren(CvAStarNode* node)
{
	int count = 6;
	for(int i = 0; i < count; i++)
	{
		CvAStarNode* check = node->m_apNeighbors[i];
		m_iTestedNodes++;

		//important things happening here
 		if(check && udFunc(udValid, node, check, 0, m_sData))
		{
			LinkChild(node, check);
		}
	}

	if(udNumExtraChildrenFunc && udGetExtraChildFunc)
	{
		int iExtraChildren = udNumExtraChildrenFunc(node, this);
		for(int i = 0; i < iExtraChildren; i++)
		{
			int x, y;
			udGetExtraChildFunc(node, i, x, y, this);
			PREFETCH_FASTAR_NODE(&(m_ppaaNodes[x][y]));

			if(isValid(x, y))
			{
				CvAStarNode* check = &(m_ppaaNodes[x][y]);

				if(check && udFunc(udValid, node, check, 0, m_sData))
				{
					LinkChild(node, check);
				}
			}
		}
	}
}

//	--------------------------------------------------------------------------------
/// Link in a child
void CvAStar::LinkChild(CvAStarNode* node, CvAStarNode* check)
{
	//we would have to start a new turn to continue
	if(node->m_iMoves == 0)
		if (node->m_iTurns+1 > m_sData.iMaxTurns) // path is getting too long ...
			return;

	//seems innocent, but is very important
	int iKnownCost = udFunc(udCost, node, check, 0, m_sData);
	if (iKnownCost == PATH_DO_NOT_USE_WEIGHT)
		return; //don't even link it up, it's a dead end

	//calculate the cumulative cost up to here
	iKnownCost += node->m_iKnownCost;

	//check termination because of total cost / normalized length
	if (m_sData.iMaxNormalizedDistance!= INT_MAX && iKnownCost > m_sData.iMaxNormalizedDistance*m_iBasicPlotCost)
		return;

	//final check. there may have been a previous path here.
	//in that case we want to keep the one with the lower total cost, which should correspond to the shorter one
	if (check->m_iKnownCost > 0 && iKnownCost > check->m_iKnownCost )
		return;

	//some bookkeeping
	m_iProcessedNodes++;

	if(check->m_eCvAStarListType == CVASTARLIST_OPEN)
	{
		node->m_apChildren.push_back(check);

		if(iKnownCost < check->m_iKnownCost)
		{
			FAssert(node->m_pParent != check);

			check->m_pParent = node;
			check->m_iKnownCost = iKnownCost;
			check->m_iTotalCost = iKnownCost + check->m_iHeuristicCost;

			UpdateOpenNode(check);
			udFunc(udNotifyChild, node, check, ASNC_OPENADD_UP, m_sData);
		}
	}
	else if(check->m_eCvAStarListType == CVASTARLIST_CLOSED)
	{
		node->m_apChildren.push_back(check);

		if(iKnownCost < check->m_iKnownCost)
		{
			FAssert(node->m_pParent != check);
			check->m_pParent = node;
			check->m_iKnownCost = iKnownCost;
			check->m_iTotalCost = iKnownCost + check->m_iHeuristicCost;
			udFunc(udNotifyChild, node, check, ASNC_CLOSEDADD_UP, m_sData);

			UpdateParents(check);
		}
	}
	else
	{
		FAssert(check->m_eCvAStarListType == NO_CVASTARLIST);
		FAssert(node->m_pParent != check);
		check->m_pParent = node;
		check->m_iKnownCost = iKnownCost;
		if(udHeuristic == NULL)
		{
			check->m_iHeuristicCost = 0;
		}
		else
		{
			check->m_iHeuristicCost = udHeuristic(check->m_iX, check->m_iY, m_iXdest, m_iYdest);
		}
		check->m_iTotalCost = check->m_iKnownCost + check->m_iHeuristicCost;

		udFunc(udNotifyChild, node, check, ASNC_NEWADD, m_sData);

		AddToOpen(check);

		node->m_apChildren.push_back(check);
	}
}

//	--------------------------------------------------------------------------------
/// Add node to open list
void CvAStar::AddToOpen(CvAStarNode* addnode)
{
	CvAStarNode* node;

	addnode->m_eCvAStarListType = CVASTARLIST_OPEN;

	if(!m_pOpen)
	{
		m_pOpen = addnode;
		m_pOpenTail = addnode;
		m_pOpen->m_pNext = NULL;
		m_pOpen->m_pPrev = NULL;

		udFunc(udNotifyList, NULL, addnode, ASNL_STARTOPEN, m_sData);

		return;
	}

	if(addnode->m_iTotalCost <= m_pOpen->m_iTotalCost)
	{
		addnode->m_pNext = m_pOpen;
		m_pOpen->m_pPrev = addnode;
		m_pOpen = addnode;

		udFunc(udNotifyList, m_pOpen->m_pNext, m_pOpen, ASNL_STARTOPEN, m_sData);
	}
	else if(addnode->m_iTotalCost >= m_pOpenTail->m_iTotalCost)
	{
		addnode->m_pPrev = m_pOpenTail;
		m_pOpenTail->m_pNext = addnode;
		m_pOpenTail = addnode;

		udFunc(udNotifyList, addnode->m_pPrev, addnode, ASNL_ADDOPEN, m_sData);
	}
	else if(abs(addnode->m_iTotalCost-m_pOpenTail->m_iTotalCost) < abs(addnode->m_iTotalCost-m_pOpen->m_iTotalCost))  //(addnode->m_iTotalCost > m_iOpenListAverage) // let's start at the end and work forwards
	{
		CvAStarNode* next;
		node = m_pOpenTail;
		next = NULL;

		while(node)
		{
			if(addnode->m_iTotalCost < node->m_iTotalCost)
			{
				next = node;
				node = node->m_pPrev;
			}
			else
			{
				if(next)
				{
					next->m_pPrev = addnode;
					addnode->m_pNext = next;
					addnode->m_pPrev = node;
					node->m_pNext = addnode;
					if(node->m_pNext == NULL)
					{
						m_pOpenTail = node;
					}

					udFunc(udNotifyList, addnode->m_pPrev, addnode, ASNL_ADDOPEN, m_sData);
				}
				else // we should just add it to the end of the list
				{
					addnode->m_pPrev = m_pOpenTail;
					m_pOpenTail->m_pNext = addnode;
					m_pOpenTail = addnode;

					udFunc(udNotifyList, addnode->m_pPrev, addnode, ASNL_ADDOPEN, m_sData);
				}

				return;
			}
		}

		// we made it to the start of this list - insert it at the beginning - we shouldn't ever get here, but...
		if (next)
		{
			next->m_pPrev = addnode;
			addnode->m_pNext = next;
			m_pOpen = addnode;

			udFunc(udNotifyList, m_pOpen->m_pNext, m_pOpen, ASNL_STARTOPEN, m_sData);
		}
	}
	else // let's start at the beginning as it should be closer
	{
		CvAStarNode* prev;
		node = m_pOpen;
		prev = NULL;

		while(node)
		{
			if(addnode->m_iTotalCost > node->m_iTotalCost)
			{
				prev = node;
				node = node->m_pNext;
			}
			else
			{
				if(prev)
				{
					prev->m_pNext = addnode;
					addnode->m_pPrev = prev;
					addnode->m_pNext = node;
					node->m_pPrev = addnode;
					if(node->m_pNext == NULL)
					{
						m_pOpenTail = node;
					}

					udFunc(udNotifyList, prev, addnode, ASNL_ADDOPEN, m_sData);
				}
				else
				{
					addnode->m_pNext = m_pOpen;
					m_pOpen->m_pPrev = addnode;
					m_pOpen = addnode;

					udFunc(udNotifyList, m_pOpen->m_pNext, m_pOpen, ASNL_STARTOPEN, m_sData);
				}

				return;
			}
		}

		// we made it to the end of this list - insert it at the end - we shouldn't ever get here, but...
		prev->m_pNext = addnode;
		addnode->m_pPrev = prev;
		m_pOpenTail = addnode;

		udFunc(udNotifyList, prev, addnode, ASNL_ADDOPEN, m_sData);
	}
}

//	--------------------------------------------------------------------------------
/// Connect in a node
void CvAStar::UpdateOpenNode(CvAStarNode* node)
{
	CvAStarNode* temp;

	FAssert(node->m_eCvAStarListType == CVASTARLIST_OPEN);

	if((node->m_pPrev != NULL) && (node->m_iTotalCost < node->m_pPrev->m_iTotalCost))
	{
		// have node free float for now
		node->m_pPrev->m_pNext = node->m_pNext;
		if(node->m_pNext)
		{
			node->m_pNext->m_pPrev = node->m_pPrev;
		}
		else
		{
			m_pOpenTail = node->m_pPrev;
		}
		// scoot down the list till we find where node goes (without connecting up as we go)
		temp = node->m_pPrev;
		while((temp != NULL) && (node->m_iTotalCost < temp->m_iTotalCost))
		{
			temp = temp->m_pPrev;
		}
		// connect node up
		if(temp != NULL)
		{
			node->m_pNext = temp->m_pNext;
			node->m_pPrev = temp;
			if(temp->m_pNext)
			{
				temp->m_pNext->m_pPrev = node;
			}
			temp->m_pNext = node;
		}
		else
		{
			node->m_pNext = m_pOpen;
			node->m_pPrev = NULL;
			if(node->m_pNext)
			{
				node->m_pNext->m_pPrev = node;
			}
			m_pOpen = node;
		}
	}
}

//	--------------------------------------------------------------------------------
/// Refresh parent node (after linking in a child)
void CvAStar::UpdateParents(CvAStarNode* node)
{
	CvAStarNode* kid;
	CvAStarNode* parent;
	int iKnownCost;
	FAssert(m_pStackHead == NULL);

	parent = node;

	while(parent != NULL)
	{
		for(size_t i = 0; i < parent->m_apChildren.size(); i++)
		{
			kid = parent->m_apChildren[i];

			iKnownCost = (parent->m_iKnownCost + udFunc(udCost, parent, kid, 0, m_sData));

			if(iKnownCost < kid->m_iKnownCost)
			{
				kid->m_iKnownCost = iKnownCost;
				kid->m_iTotalCost = kid->m_iKnownCost + kid->m_iHeuristicCost;
				FAssert(parent->m_pParent != kid);
				kid->m_pParent = parent;
				if(kid->m_eCvAStarListType == CVASTARLIST_OPEN)
				{
					UpdateOpenNode(kid);
				}
				udFunc(udNotifyChild, parent, kid, ASNC_PARENTADD_UP, m_sData);

				Push(kid);
			}
		}

		parent = Pop();
	}
}

//	--------------------------------------------------------------------------------
/// Push a node on the stack
void CvAStar::Push(CvAStarNode* node)
{
	if(node->m_bOnStack)
	{
		return;
	}

	if(m_pStackHead == NULL)
	{
		m_pStackHead = &(m_ppaaNodes[node->m_iX][node->m_iY]);
	}
	else
	{
		FAssert(node->m_pStack == NULL);
		node->m_pStack = m_pStackHead;
		m_pStackHead = node;
	}

	node->m_bOnStack = true;
}

//	--------------------------------------------------------------------------------
/// Pop a node from the stack
CvAStarNode* CvAStar::Pop()
{
	CvAStarNode* node;

	if(m_pStackHead == NULL)
	{
		return NULL;
	}

	node = m_pStackHead;
	m_pStackHead = m_pStackHead->m_pStack;
	node->m_pStack = NULL;

	node->m_bOnStack = false;

	return node;
}


//C-STYLE NON-MEMBER FUNCTIONS

//-------------------------------------------------------------------------------------
// A structure holding some unit values that are invariant during a path plan operation
struct UnitPathCacheData
{
	CvUnit* pUnit;

	int m_aBaseMoves[NUM_DOMAIN_TYPES];
	int m_iMaxMoves;
	PlayerTypes m_ePlayerID;
	TeamTypes m_eTeamID;
	DomainTypes m_eDomainType;

	bool m_bAIControl;
	bool m_bIsImmobile;
	bool m_bIsNoRevealMap;
	bool m_bCanEverEmbark;
	bool m_bIsEmbarked;
	bool m_bCanAttack;
	bool m_bDoDanger;
	inline bool DoDanger() const { return m_bDoDanger; }

	inline int baseMoves(DomainTypes eType) const { return m_aBaseMoves[eType]; }
	inline int maxMoves() const { return m_iMaxMoves; }
	inline PlayerTypes getOwner() const { return m_ePlayerID; }
	inline TeamTypes getTeam() const { return m_eTeamID; }
	inline DomainTypes getDomainType() const { return m_eDomainType; }
	inline bool isAIControl() const { return m_bAIControl; }
	inline bool IsImmobile() const { return m_bIsImmobile; }
	inline bool isNoRevealMap() const { return m_bIsNoRevealMap; }
	inline bool CanEverEmbark() const { return m_bCanEverEmbark; }
	inline bool isEmbarked() const { return m_bIsEmbarked; }
	inline bool IsCanAttack() const { return m_bCanAttack; }
};

//-------------------------------------------------------------------------------------
// get all information which is constant during a path planning operation
void UnitPathInitialize(const SPathFinderUserData& data, CvAStar* finder)
{
	UnitPathCacheData* pCacheData = reinterpret_cast<UnitPathCacheData*>(finder->GetScratchBufferDirty());

	CvUnit* pUnit = GET_PLAYER(data.ePlayer).getUnit(data.iUnitID);
	pCacheData->pUnit = pUnit;

	for (int i = 0; i < NUM_DOMAIN_TYPES; ++i)
		pCacheData->m_aBaseMoves[i] = pUnit->baseMoves((DomainTypes)i);

	pCacheData->m_iMaxMoves = pUnit->maxMoves();
	pCacheData->m_ePlayerID = pUnit->getOwner();
	pCacheData->m_eTeamID = pUnit->getTeam();
	pCacheData->m_eDomainType = pUnit->getDomainType();
	pCacheData->m_bAIControl = !pUnit->isHuman() || pUnit->IsAutomated();
	pCacheData->m_bIsImmobile = pUnit->IsImmobile();
	pCacheData->m_bIsNoRevealMap = pUnit->isNoRevealMap();
	pCacheData->m_bCanEverEmbark = pUnit->CanEverEmbark();
	pCacheData->m_bIsEmbarked = pUnit->isEmbarked();
	pCacheData->m_bCanAttack = pUnit->IsCanAttack();
	//danger is relevant for AI controlled units, if we didn't explicitly disable it
	pCacheData->m_bDoDanger = pCacheData->m_bAIControl && !finder->HaveFlag(CvUnit::MOVEFLAG_IGNORE_DANGER);
}

//	--------------------------------------------------------------------------------
void UnitPathUninitialize(const SPathFinderUserData&, CvAStar*)
{

}

//-------------------------------------------------------------------------------------
// get all information which depends on a particular node. 
// this is versioned, so we don't need to recalculate during the same pathfinding operation
void UpdateNodeCacheData(CvAStarNode* node, const CvUnit* pUnit, bool bDoDanger, const CvAStar* finder)
{
	if (!node || !pUnit)
		return;

	CvPathNodeCacheData& kToNodeCacheData = node->m_kCostCacheData;
	if (kToNodeCacheData.iGenerationID==finder->GetCurrentGenerationID())
		return;

	const CvPlot* pPlot = GC.getMap().plotUnchecked(node->m_iX, node->m_iY);
	TeamTypes eUnitTeam = pUnit->getTeam();
	CvTeam& kUnitTeam = GET_TEAM(eUnitTeam);
	TeamTypes ePlotTeam = pPlot->getTeam();

	kToNodeCacheData.bIsRevealedToTeam = pPlot->isRevealed(eUnitTeam);
	kToNodeCacheData.bPlotVisibleToTeam = pPlot->isVisible(eUnitTeam);
	kToNodeCacheData.bIsEndTurnPlot = pPlot->isValidEndTurnPlot(pUnit->getOwner());
	kToNodeCacheData.bIsWater = (pPlot->isWater() && !pPlot->IsAllowsWalkWater());
#if defined(MOD_PATHFINDER_TERRAFIRMA)
	kToNodeCacheData.bIsTerraFirma = pPlot->isTerraFirma(pUnit) && !pPlot->IsAllowsWalkWater();
#endif

	kToNodeCacheData.bCanEnterTerrain = pUnit->canEnterTerrain(*pPlot, CvUnit::MOVEFLAG_PRETEND_CORRECT_EMBARK_STATE);
	kToNodeCacheData.bCanEnterTerritory = pUnit->canEnterTerritory(ePlotTeam,false,pPlot->isCity(),finder->HaveFlag(CvUnit::MOVEFLAG_DECLARE_WAR));

	kToNodeCacheData.bContainsOtherFriendlyTeamCity = false;
	CvCity* pCity = pPlot->getPlotCity();
	if (pCity)
	{
		if (pUnit->getOwner() != pCity->getOwner() && !kUnitTeam.isAtWar(pCity->getTeam()))
			kToNodeCacheData.bContainsOtherFriendlyTeamCity = true;
	}
	kToNodeCacheData.bContainsEnemyCity = pPlot->isEnemyCity(*pUnit);
	if (kToNodeCacheData.bPlotVisibleToTeam)
	{
		kToNodeCacheData.bContainsVisibleEnemy = pPlot->isVisibleEnemyUnit(pUnit);
		kToNodeCacheData.bContainsVisibleEnemyDefender = pPlot->isVisibleEnemyDefender(pUnit);
	}
	else
	{
		kToNodeCacheData.bContainsVisibleEnemy = false;
		kToNodeCacheData.bContainsVisibleEnemyDefender = false;
	}

	kToNodeCacheData.bUnitLimitReached = (pPlot->getNumFriendlyUnitsOfType(pUnit) >= pPlot->getUnitLimit());

	if (bDoDanger)
		kToNodeCacheData.iPlotDanger = GET_PLAYER(pUnit->getOwner()).GetPlotDanger(*pPlot, pUnit);
	else
		kToNodeCacheData.iPlotDanger = 0;

	//done!
	kToNodeCacheData.iGenerationID = finder->GetCurrentGenerationID();
}

//	--------------------------------------------------------------------------------
int PathDest(int iToX, int iToY, const SPathFinderUserData&, const CvAStar* finder)
{
	if(iToX == finder->GetDestX() && iToY == finder->GetDestY())
	{
		return true;
	}
	else
	{
		return false;
	}
}


//	--------------------------------------------------------------------------------
/// Standard path finder - is this end point for the path valid?
int PathDestValidGeneric(int iToX, int iToY, const SPathFinderUserData&, const CvAStar* finder, bool bCheckStacking)
{
	CvPlot* pToPlot = GC.getMap().plotCheckInvalid(iToX, iToY);
	FAssert(pToPlot != NULL);

	//do not use the node data cache here yet, only the unit data cache
	const UnitPathCacheData* pCacheData = reinterpret_cast<const UnitPathCacheData*>(finder->GetScratchBuffer());
	CvUnit* pUnit = pCacheData->pUnit;
	TeamTypes eTeam = pCacheData->getTeam();

	if(pToPlot == NULL || pUnit == NULL)
		return FALSE;

	if(pUnit->plot() == pToPlot)
	{
		return TRUE;
	}

	if(pCacheData->IsImmobile())
	{
		return FALSE;
	}

	//checks which need visibility (logically so we don't leak information)
	if (pToPlot->isVisible(eTeam))
	{
		// assume that we can change our embarking state
		int iMoveFlags = CvUnit::MOVEFLAG_DESTINATION | CvUnit::MOVEFLAG_PRETEND_CORRECT_EMBARK_STATE;

		if(pUnit->IsDeclareWar() || finder->HaveFlag(CvUnit::MOVEFLAG_DECLARE_WAR))
		{
			iMoveFlags |= CvUnit::MOVEFLAG_DECLARE_WAR;
		}

		if(!bCheckStacking || finder->HaveFlag(CvUnit::MOVEFLAG_IGNORE_STACKING)) 
		{
			iMoveFlags |= CvUnit::MOVEFLAG_IGNORE_STACKING;
		}

		// all other rules are hidden here
		if (pUnit->isRanged())
		{
			//ranged units can only "attack" civilian by moving (i.e. capture them)
			if (pToPlot->isVisibleEnemyUnit(pUnit) && !pToPlot->isVisibleEnemyDefender(pUnit))
				iMoveFlags |= CvUnit::MOVEFLAG_ATTACK;

			if (!pUnit->canMoveInto(*pToPlot, iMoveFlags))
				return FALSE;
		}
		else
		{
			if (!pUnit->canMoveOrAttackInto(*pToPlot, iMoveFlags))
				return FALSE;
		}
	}

	//checks which need a revealed plot (logically so we don't leak information)
	if (pToPlot->isRevealed(eTeam))
	{
		//check terrain and territory - only if not visible, otherwise it has been checked above already
		if (!pToPlot->isVisible(eTeam))
		{
			if(!pUnit->canEnterTerrain(*pToPlot,CvUnit::MOVEFLAG_PRETEND_CORRECT_EMBARK_STATE))
				return FALSE;

			if(!pUnit->canEnterTerritory(eTeam,false,pToPlot->isCity(),finder->HaveFlag(CvUnit::MOVEFLAG_DECLARE_WAR)))
				return FALSE;
		}

		//cannot end turn e.g. on top of a mountain (even if it's passable)
		if(!pToPlot->isValidEndTurnPlot(pUnit->getOwner()))
			return FALSE;

		if (finder->HaveFlag(CvUnit::MOVEFLAG_NO_EMBARK) && pToPlot->isWater() && !pToPlot->IsAllowsWalkWater())
			return FALSE;

		if(pUnit->IsCombatUnit())
		{
			CvCity* pCity = pToPlot->getPlotCity();
			if(pCity)
			{
				if(pCacheData->getOwner() != pCity->getOwner() && !GET_TEAM(eTeam).isAtWar(pCity->getTeam()))
				{
					return FALSE;
				}
			}
		}
	}
	else
	{
		if(pCacheData->isNoRevealMap())
		{
			return FALSE;
		}
	}

	return TRUE;
}


int PathDestValid(int iToX, int iToY, const SPathFinderUserData& data, const CvAStar* finder)
{
	return PathDestValidGeneric(iToX,iToY,data,finder,true);
}

int IgnoreUnitsDestValid(int iToX, int iToY, const SPathFinderUserData& data, const CvAStar* finder)
{
	return PathDestValidGeneric(iToX,iToY,data,finder,false);
}

//	--------------------------------------------------------------------------------
/// Standard path finder - determine heuristic cost
int PathHeuristic(int iFromX, int iFromY, int iToX, int iToY)
{
	return (plotDistance(iFromX, iFromY, iToX, iToY) * PATH_MOVEMENT_WEIGHT*30); //a normal move is 60 times the base cost!
}

//	--------------------------------------------------------------------------------
/// Standard path finder - compute cost of a path
int PathCostGeneric(const CvAStarNode* parent, CvAStarNode* node, int, const SPathFinderUserData&, const CvAStar* finder, bool bWithZOC)
{
	int iStartMoves = parent->m_iMoves;
	int iTurns = parent->m_iTurns;

	CvMap& kMap = GC.getMap();
	int iFromPlotX = parent->m_iX;
	int iFromPlotY = parent->m_iY;
	CvPlot* pFromPlot = kMap.plotUnchecked(iFromPlotX, iFromPlotY);

	int iToPlotX = node->m_iX;
	int iToPlotY = node->m_iY;
	CvPlot* pToPlot = kMap.plotUnchecked(iToPlotX, iToPlotY);
	bool bIsPathDest = finder->IsPathDest(iToPlotX, iToPlotY);

	const UnitPathCacheData* pCacheData = reinterpret_cast<const UnitPathCacheData*>(finder->GetScratchBuffer());
	CvUnit* pUnit = pCacheData->pUnit;

	TeamTypes eUnitTeam = pCacheData->getTeam();
	DomainTypes eUnitDomain = pCacheData->getDomainType();
	CvAssertMsg(eUnitDomain != DOMAIN_AIR, "pUnit->getDomainType() is not expected to be equal with DOMAIN_AIR");

#if defined(MOD_PATHFINDER_TERRAFIRMA)
	bool bToPlotIsWater = !pToPlot->isTerraFirma(pUnit) && !pToPlot->IsAllowsWalkWater();
	bool bFromPlotIsWater = !pFromPlot->isTerraFirma(pUnit) && !pFromPlot->IsAllowsWalkWater();
#else
	bool bToPlotIsWater = pToPlot->isWater() && !pToPlot->IsAllowsWalkWater();
#endif

	//if we would have to start a new turn
	if (iStartMoves==0)
	{
		// inconspicuous but important
		iTurns++;

		if (CvUnitMovement::ConsumesAllMoves(pUnit, pFromPlot, pToPlot) || (bWithZOC && CvUnitMovement::IsSlowedByZOC(pUnit, pFromPlot, pToPlot)))
		{
			// The movement would consume all moves, get the moves we will forfeit based on the source plot, rather than
			// the destination plot.  This fixes issues where a land unit that has more movement points on water than on land
			// would have a very high cost to move onto water if their first move of the turn was at the edge of the water.
			iStartMoves = pCacheData->baseMoves(bFromPlotIsWater?DOMAIN_SEA:DOMAIN_LAND) * GC.getMOVE_DENOMINATOR();
		}
		else
			iStartMoves = pCacheData->baseMoves(bToPlotIsWater?DOMAIN_SEA:DOMAIN_LAND) * GC.getMOVE_DENOMINATOR();
	}

	// Get the cost of moving to the new plot, passing in our max moves or the moves we have left, in case the movementCost 
	// method wants to burn all our remaining moves.  This is needed because our remaining moves for this segment of the path
	// may be larger or smaller than the baseMoves if some moves have already been used or if the starting domain (LAND/SEA)
	// of the path segment is different from the destination plot.
	DomainTypes eDomain = (pToPlot->isWater() || pCacheData->isEmbarked()) ? DOMAIN_SEA : eUnitDomain;
	int iMovementCost = 0;
	
	// do not pass in the remaining moves, we want to see the true cost!
	if (bWithZOC)
		iMovementCost = CvUnitMovement::MovementCost(pUnit, pFromPlot, pToPlot, pCacheData->baseMoves(eDomain), pCacheData->maxMoves());
	else
		iMovementCost = CvUnitMovement::MovementCostNoZOC(pUnit, pFromPlot, pToPlot, pCacheData->baseMoves(eDomain), pCacheData->maxMoves());

	// Is the cost greater than our max?
	int iMovesLeft = iStartMoves - iMovementCost;
	if (iMovesLeft < 0)
	{
		// Yes, we will still let the move happen, but that is the end of the turn (handled in the next round when iStartMoves==0)
		iMovesLeft = 0;

		// We pay only for what we take
		iMovementCost = iStartMoves;
	}

	//check again whether we already found a better path now that we know the movement cost
	if (node->m_iTurns>0)
	{
		//this should already be handled in PathValid, but you never know
		if (node->m_iTurns<iTurns)
			return PATH_DO_NOT_USE_WEIGHT;

		if (node->m_iTurns==iTurns && node->m_iMoves>iMovesLeft)
			return PATH_DO_NOT_USE_WEIGHT;
	}

	//important: store the remaining moves in the node so we don't have to recalculate later (e.g. in PathAdd)
	//with all the checks before, we know that this is the best we can currently do. update of the costs is done LinkChild
	node->m_iMoves = iMovesLeft;
	node->m_iTurns = iTurns;

	//base cost
	int iCost = (PATH_MOVEMENT_WEIGHT * iMovementCost);

	//experimental: burn off superfluous movement points at the destination
	//they usually go to waste, so we would like to invest them in a better path instead
	//however, we leave some for a possible attack
	if (bIsPathDest)
		iCost += PATH_MOVEMENT_BURN_WEIGHT * max(0, iMovesLeft-GC.getMOVE_DENOMINATOR()/2);

	//extra cost for ending the turn on various types of undesirable plots (unless explicitly requested)
	if(iMovesLeft == 0 && !bIsPathDest)
	{
		if(pUnit->IsCombatUnit())
		{
			iCost += (PATH_DEFENSE_WEIGHT * std::max(0, (200 - ((pUnit->noDefensiveBonus()) ? 0 : pToPlot->defenseModifier(eUnitTeam, false)))));
		}

		if(finder->HaveFlag(CvUnit::MOVEFLAG_MAXIMIZE_EXPLORE))
		{
			if(!pToPlot->isHills())
			{
				iCost += PATH_EXPLORE_NON_HILL_WEIGHT;
			}
		}

		// Damage caused by features (mods)
		if(0 != GC.getPATH_DAMAGE_WEIGHT())
		{
			if(pToPlot->getFeatureType() != NO_FEATURE)
			{
#if defined(MOD_API_PLOT_BASED_DAMAGE)
				if (MOD_API_PLOT_BASED_DAMAGE) {
					iCost += (GC.getPATH_DAMAGE_WEIGHT() * std::max(0, pToPlot->getTurnDamage(pUnit->ignoreTerrainDamage(), pUnit->ignoreFeatureDamage(), pUnit->extraTerrainDamage(), pUnit->extraFeatureDamage()))) / GC.getMAX_HIT_POINTS();
				} else {
#endif
					iCost += (GC.getPATH_DAMAGE_WEIGHT() * std::max(0, GC.getFeatureInfo(pToPlot->getFeatureType())->getTurnDamage())) / GC.getMAX_HIT_POINTS();
#if defined(MOD_API_PLOT_BASED_DAMAGE)
				}
#endif
			}

			if(pToPlot->getExtraMovePathCost() > 0)
			{
				iCost += (PATH_MOVEMENT_WEIGHT * pToPlot->getExtraMovePathCost());
			}
		}

		if (pUnit->isHasPromotion((PromotionTypes)GC.getPROMOTION_UNWELCOME_EVANGELIST()))
		{
			// Avoid being in a territory that we are not welcome in, unless the human is manually controlling the unit.
			if (pCacheData->isAIControl())
			{
				PlayerTypes ePlotOwner = pToPlot->getOwner();
				TeamTypes ePlotTeam = pToPlot->getTeam();
				if (ePlotTeam != NO_TEAM && !GET_PLAYER(ePlotOwner).isMinorCiv() && ePlotTeam!=pCacheData->getTeam() && !GET_TEAM(ePlotTeam).IsAllowsOpenBordersToTeam(pCacheData->getTeam()))
				{
					iCost += PATH_END_TURN_MISSIONARY_OTHER_TERRITORY;
				}
			}
		}
		else
		{
			if(pToPlot->getTeam() != eUnitTeam)
			{
				iCost += PATH_TERRITORY_WEIGHT;
			}
		}

		// If we are a land unit and we are ending the turn on water, make the cost a little higher so that
		// we favor staying on land or getting back to land as quickly as possible because it is dangerous to
		// be on the water.  Don't add this penalty if the unit is human controlled however, we will assume they want
		// the best path, rather than the safest.
		if(eUnitDomain == DOMAIN_LAND && bToPlotIsWater && pCacheData->isAIControl())
		{
			iCost += PATH_THROUGH_WATER;
		}

		//danger check
		if ( pCacheData->DoDanger() )
		{
			//note: this includes an overkill factor because usually not all enemy units will attack this one unit
			int iPlotDanger = node->m_kCostCacheData.iPlotDanger;

			if (pUnit->IsCombatUnit())
			{
				//combat units can still tolerate some danger
				//embarkation is handled implicitly because danger value will be higher
				if (iPlotDanger >= pUnit->GetCurrHitPoints()*3)
					iCost += PATH_END_TURN_MORTAL_DANGER_WEIGHT;
				else if (iPlotDanger >= pUnit->GetCurrHitPoints())
					iCost += PATH_END_TURN_HIGH_DANGER_WEIGHT;
				else
					iCost += PATH_END_TURN_LOW_DANGER_WEIGHT;
			}
			else //civilian
			{
				//danger usually means capture (INT_MAX), unless embarked
				if (iPlotDanger > pUnit->GetCurrHitPoints())
					iCost += PATH_END_TURN_MORTAL_DANGER_WEIGHT;
				else
					iCost += PATH_END_TURN_LOW_DANGER_WEIGHT;
			}
		}
	}

	if(finder->HaveFlag(CvUnit::MOVEFLAG_MAXIMIZE_EXPLORE))
	{
		int iUnseenPlots = pToPlot->getNumAdjacentNonrevealed(eUnitTeam);
		if(!pToPlot->isRevealed(eUnitTeam))
		{
			iUnseenPlots += 1;
		}

		iCost += (7 - iUnseenPlots) * PATH_EXPLORE_NON_REVEAL_WEIGHT;
	}

	if(pCacheData->IsCanAttack() && bIsPathDest)
	{
		if(node->m_kCostCacheData.bContainsVisibleEnemyDefender)
		{
			iCost += (PATH_DEFENSE_WEIGHT * std::max(0, (200 - ((pUnit->noDefensiveBonus()) ? 0 : pFromPlot->defenseModifier(eUnitTeam, false)))));

			if(!(pUnit->isRiverCrossingNoPenalty()))
			{
				if(pFromPlot->isRiverCrossing(directionXY(pFromPlot, pToPlot)))
				{
					iCost += (PATH_RIVER_WEIGHT * -(GC.getRIVER_ATTACK_MODIFIER()));
					iCost += (PATH_MOVEMENT_WEIGHT * iMovesLeft);
				}
			}
		}
	}

	iCost += PATH_STEP_WEIGHT;
	return iCost;
}

int PathCost(const CvAStarNode* parent, CvAStarNode* node, int, const SPathFinderUserData& data, const CvAStar* finder)
{
	return PathCostGeneric(parent,node,0,data,finder,true);
}

int IgnoreUnitsCost(const CvAStarNode* parent, CvAStarNode* node, int, const SPathFinderUserData& data, const CvAStar* finder)
{
	return PathCostGeneric(parent,node,0,data,finder,false);
}

//	---------------------------------------------------------------------------
/// Standard path finder - check validity of a coordinate
int PathValidGeneric(const CvAStarNode* parent, const CvAStarNode* node, int, const SPathFinderUserData&, const CvAStar* finder, bool bCheckStacking)
{
	// If this is the first node in the path, it is always valid (starting location)
	if (parent == NULL)
		return TRUE;

	//check if we already found a better way ...
	if (node->m_iTurns>0)
	{
		if (node->m_iTurns<parent->m_iTurns)
			return FALSE;

		if (node->m_iTurns==parent->m_iTurns && node->m_iMoves>parent->m_iMoves)
			return FALSE;
	}

	// Cached values for this node that we will use
	const CvPathNodeCacheData& kToNodeCacheData = node->m_kCostCacheData;
	const CvPathNodeCacheData& kFromNodeCacheData = parent->m_kCostCacheData;
	const UnitPathCacheData* pCacheData = reinterpret_cast<const UnitPathCacheData*>(finder->GetScratchBuffer());
	CvUnit* pUnit = pCacheData->pUnit;
	TeamTypes eUnitTeam = pCacheData->getTeam();

	//overwrite the parameter we're given if necessary
	bCheckStacking	= bCheckStacking && !finder->HaveFlag(CvUnit::MOVEFLAG_IGNORE_STACKING);

	// We have determined that this node is not the origin above, so parent != NULL
	bool bNewTurn = (parent->m_iMoves==0);
	bool bNextNodeHostile = kToNodeCacheData.bContainsEnemyCity || kToNodeCacheData.bContainsVisibleEnemyDefender;
	bool bNextNodeVisibleToTeam = kToNodeCacheData.bPlotVisibleToTeam;

	// we would start a new turn or run into an enemy or run into unknown territory, so we must be able to end the turn on the parent plot
	// the bNewTurn check should really be done when validating the parent node, but at that point we don't know the movement cost yet (PathCost is called after PathValid)
	if (bNewTurn || bNextNodeHostile || !bNextNodeVisibleToTeam)
	{
		if (kFromNodeCacheData.bPlotVisibleToTeam)
		{
			// check stacking (if visible)
			if(bCheckStacking && kFromNodeCacheData.bUnitLimitReached)
			{
				// Don't count origin, or else a unit will block its own movement!
				if(parent->m_iX != pUnit->getX() || parent->m_iY != pUnit->getY())
					return FALSE;
			}
		}

		if (kFromNodeCacheData.bIsRevealedToTeam)
		{
#if defined(MOD_GLOBAL_BREAK_CIVILIAN_RESTRICTIONS)
			if(!MOD_GLOBAL_BREAK_CIVILIAN_RESTRICTIONS || pCacheData->m_bCanAttack)
#else
			if(true)
#endif
			{
				if (kFromNodeCacheData.bContainsOtherFriendlyTeamCity)
					return FALSE;
			}

			// don't ever stop on a mountain (even if it's passable)
			if(!kFromNodeCacheData.bIsEndTurnPlot)
				return FALSE;
		}
	}

	CvMap& theMap = GC.getMap();
	CvPlot* pFromPlot = theMap.plotUnchecked(parent->m_iX, parent->m_iY);
	CvPlot* pToPlot = theMap.plotUnchecked(node->m_iX, node->m_iY);

	//some checks about units etc. they need to be visible, we leak information
	if (kToNodeCacheData.bPlotVisibleToTeam)
	{
		int iMoveFlags = CvUnit::MOVEFLAG_PRETEND_CORRECT_EMBARK_STATE;
		if(!bCheckStacking || finder->HaveFlag(CvUnit::MOVEFLAG_IGNORE_STACKING)) 
		{
			iMoveFlags |= CvUnit::MOVEFLAG_IGNORE_STACKING;
		}

		//special checks for last node - similar as in PathDestValid
		if (finder->IsPathDest(node->m_iX,node->m_iY))
		{
			iMoveFlags |= CvUnit::MOVEFLAG_DESTINATION;

			//check friendly stacking - this is also checked in canMoveInto but this way it's maybe faster
			if(bCheckStacking && kToNodeCacheData.bUnitLimitReached)
				return FALSE;

			//all other rules are hidden here
			if (pUnit->isRanged())
			{
				//ranged units can only "attack" civilian by moving (i.e. capture them)
				if (kToNodeCacheData.bContainsVisibleEnemy && !kToNodeCacheData.bContainsVisibleEnemyDefender)
					iMoveFlags |= CvUnit::MOVEFLAG_ATTACK;

				if (!pUnit->canMoveInto(*pToPlot, iMoveFlags))
					return FALSE;
			}
			else
			{
				if (!pUnit->canMoveOrAttackInto(*pToPlot, iMoveFlags))
					return FALSE;
			}
		}
		else
		{
			//normal "along the way" plot
			if(!pUnit->canMoveInto(*pToPlot, iMoveFlags))
				return FALSE;
		}
	}

	//some checks about terrain etc. needs to be revealed, otherwise we leak information
	if (kToNodeCacheData.bIsRevealedToTeam)
	{
		// check impassable terrain - in case this plot is visible we already checked in canMoveInto, but it should be fast
		if(!kToNodeCacheData.bCanEnterTerrain)
			return FALSE;

		if(!kToNodeCacheData.bCanEnterTerritory)
			return FALSE;

		if(pCacheData->getDomainType() == DOMAIN_LAND)
		{
			if (finder->HaveFlag(CvUnit::MOVEFLAG_NO_EMBARK) && kToNodeCacheData.bIsWater && kFromNodeCacheData.bIsTerraFirma)
			{
				return FALSE;
			}

#if defined(MOD_PATHFINDER_TERRAFIRMA)
			if( kFromNodeCacheData.bIsTerraFirma && !kToNodeCacheData.bIsTerraFirma && kToNodeCacheData.bIsRevealedToTeam && !pUnit->canEmbarkOnto(*pFromPlot, *pToPlot, true))
#else
			if(!kFromNodeCacheData.bIsWater && kToNodeCacheData.bIsWater && kToNodeCacheData.bIsRevealedToTeam && !pUnit->canEmbarkOnto(*pFromPlot, *pToPlot, true))
#endif
			{
#if defined(MOD_PATHFINDER_DEEP_WATER_EMBARKATION)
				if(!pUnit->canMoveAllTerrain() && !pToPlot->IsAllowsWalkWater())
#else
				if(!pUnit->IsHoveringUnit() && !pUnit->canMoveAllTerrain() && !pToPlot->IsAllowsWalkWater())
#endif
				{
					return FALSE;
				}
			}
		}

		//normally we would be able to enter enemy territory if at war
		if(finder->HaveFlag(CvUnit::MOVEFLAG_TERRITORY_NO_ENEMY))
		{
			if(pToPlot->isOwned())
			{
				if(atWar(pToPlot->getTeam(), eUnitTeam))
				{
					return FALSE;
				}
			}
		}

		//ocean allowed?
		if ( finder->HaveFlag(CvUnit::MOVEFLAG_NO_OCEAN) )
		{
			if (pToPlot->getTerrainType() == TERRAIN_OCEAN)
			{
				return FALSE;
			}
		}
	}

	return TRUE;
}

int PathValid(const CvAStarNode* parent, const CvAStarNode* node, int, const SPathFinderUserData& data, const CvAStar* finder)
{
	return PathValidGeneric(parent,node,0,data,finder,true);
}

int IgnoreUnitsValid(const CvAStarNode* parent, const CvAStarNode* node, int, const SPathFinderUserData& data, const CvAStar* finder)
{
	return PathValidGeneric(parent,node,0,data,finder,false);
}

//	--------------------------------------------------------------------------------
/// Standard path finder - add a new path
int PathAdd(CvAStarNode*, CvAStarNode* node, int operation, const SPathFinderUserData&, CvAStar* finder)
{
	const UnitPathCacheData* pCacheData = reinterpret_cast<const UnitPathCacheData*>(finder->GetScratchBuffer());
	const CvUnit* pUnit = pCacheData->pUnit;

	if(operation == ASNC_INITIALADD)
	{
		//in this case we did not call PathCost() before, so we have to set the initial values here
		node->m_iMoves = pUnit->movesLeft();
		node->m_iTurns = 1;

		UpdateNodeCacheData(node,pUnit,pCacheData->DoDanger(),finder);
	}

	//update cache for all possible children
	for(int i = 0; i < 6; i++)
	{
		CvAStarNode* neighbor = node->m_apNeighbors[i];
		UpdateNodeCacheData(neighbor,pUnit,pCacheData->DoDanger(),finder);
	}

	for(int i = 0; i < finder->GetNumExtraChildren(node); i++)
	{
		CvAStarNode* neighbor = finder->GetExtraChild(node,i);
		UpdateNodeCacheData(neighbor,pUnit,pCacheData->DoDanger(),finder);
	}

	return 1;
}

//	--------------------------------------------------------------------------------
/// Two layer path finder - if add a new open node with movement left, add a second one assuming stop for turn here
int PathNodeAdd(CvAStarNode* /*parent*/, CvAStarNode* node, int operation, const SPathFinderUserData&, CvAStar* finder)
{
	if(operation == ASNL_ADDOPEN || operation == ASNL_STARTOPEN)
	{
		// Are there movement points left and we're worried about stacking?
		// Also: never stop on a mountain, even if it's passable
		if(node->m_iMoves > 0 && !finder->IsPathDest(node->m_iX, node->m_iY) && !finder->IsPathStart(node->m_iX, node->m_iY) && 
			!finder->HaveFlag(CvUnit::MOVEFLAG_IGNORE_STACKING) && node->m_kCostCacheData.bIsEndTurnPlot)
		{
			// Retrieve the secondary node
			CvTwoLayerPathFinder* twoLayerFinder = static_cast<CvTwoLayerPathFinder*>(finder);
			CvAStarNode* pNode2 = twoLayerFinder->GetPartialMoveNode(node->m_iX, node->m_iY);

			//assume a stop here
			pNode2->m_iMoves = 0;
			pNode2->m_iTurns = node->m_iTurns;
			pNode2->m_iHeuristicCost = node->m_iHeuristicCost;
			//but wasting movement points is bad
			pNode2->m_iKnownCost = node->m_iKnownCost + (PATH_MOVEMENT_WEIGHT * node->m_iMoves);

			//make sure it's a good idea to stop here ...
			const UnitPathCacheData* pCacheData = reinterpret_cast<const UnitPathCacheData*>(finder->GetScratchBuffer());
			if(pCacheData->DoDanger())
				if (node->m_kCostCacheData.iPlotDanger > pCacheData->pUnit->GetCurrHitPoints()*2)
					pNode2->m_iKnownCost += PATH_END_TURN_MORTAL_DANGER_WEIGHT;

			//we sort the nodes by total cost!
			pNode2->m_iTotalCost = pNode2->m_iKnownCost + pNode2->m_iHeuristicCost;

			pNode2->m_iX = node->m_iX;
			pNode2->m_iY = node->m_iY;
			pNode2->m_pParent = node->m_pParent;
			pNode2->m_eCvAStarListType = CVASTARLIST_OPEN;
			pNode2->m_kCostCacheData = node->m_kCostCacheData;
			finder->AddToOpen(pNode2);
		}
	}

	return 1;
}

//	--------------------------------------------------------------------------------
/// Step path finder - is this end point for the path valid?
int StepDestValid(int iToX, int iToY, const SPathFinderUserData&, const CvAStar* finder)
{
	CvPlot* pFromPlot;
	CvPlot* pToPlot;

	CvMap& kMap = GC.getMap();
	pFromPlot = kMap.plotUnchecked(finder->GetStartX(), finder->GetStartY());
	pToPlot = kMap.plotUnchecked(iToX, iToY);

	if(pFromPlot->getArea() != pToPlot->getArea())
	{
		return FALSE;
	}

	return TRUE;
}

//	--------------------------------------------------------------------------------
/// Step path finder - determine heuristic cost
int StepHeuristic(int iFromX, int iFromY, int iToX, int iToY)
{
	return plotDistance(iFromX, iFromY, iToX, iToY);
}


//	--------------------------------------------------------------------------------
/// Step path finder - compute cost of a path
int StepCost(const CvAStarNode*, CvAStarNode* node, int, const SPathFinderUserData&, const CvAStar*)
{
	CvPlot* pNewPlot = GC.getMap().plotUnchecked(node->m_iX, node->m_iY);
	
	return pNewPlot->isRoughGround() ? 2 : 1;
}


//	--------------------------------------------------------------------------------
/// Step path finder - check validity of a coordinate
int StepValidGeneric(const CvAStarNode* parent, const CvAStarNode* node, int, const SPathFinderUserData& data, const CvAStar* finder, bool bAnyArea, bool bWide)
{
	if(parent == NULL)
		return TRUE;

	PlayerTypes ePlayer = data.ePlayer;
	PlayerTypes eEnemy = (PlayerTypes)data.iTypeParameter; //we pretend we can enter this player's plots even if we're not at war

	CvMap& kMap = GC.getMap();
	CvPlot* pToPlot = kMap.plotUnchecked(node->m_iX, node->m_iY);
	CvPlot* pFromPlot = kMap.plotUnchecked(parent->m_iX, parent->m_iY);

	if (!pFromPlot || !pToPlot)
		return FALSE;

	//this is the important check here - stay within the same area
	if(!bAnyArea && pFromPlot->getArea() != pToPlot->getArea())
		return FALSE;

	//if we have a given player, check their particular impassability (depends on techs etc)
	if(!pToPlot->isValidMovePlot(ePlayer))
		return FALSE;

	//are we allowed to use ocean plots?
	if (finder->HaveFlag(CvUnit::MOVEFLAG_NO_OCEAN) && pToPlot->getTerrainType() == TERRAIN_OCEAN)
		return FALSE;

	//territory check
	PlayerTypes ePlotOwnerPlayer = pToPlot->getOwner();
	if (ePlotOwnerPlayer != NO_PLAYER && ePlayer != NO_PLAYER && ePlotOwnerPlayer != eEnemy && !pToPlot->IsFriendlyTerritory(ePlayer))
	{
		CvPlayer& plotOwnerPlayer = GET_PLAYER(ePlotOwnerPlayer);
		bool bPlotOwnerIsMinor = plotOwnerPlayer.isMinorCiv();

		if(!bPlotOwnerIsMinor)
		{
			TeamTypes eMyTeam = GET_PLAYER(ePlayer).getTeam();
			TeamTypes ePlotOwnerTeam = plotOwnerPlayer.getTeam();

			if(!atWar(eMyTeam, ePlotOwnerTeam))
			{
				return FALSE;
			}
		}
	}

	//for multi-unit formations it makes sense to have a wide path
	if (bWide)
	{
		//direction looking backward!
		DirectionTypes eRear = directionXY(pToPlot,pFromPlot);

		int eRearLeft = (int(eRear) + 5) % 6; 
		int eRearRight = (int(eRear) + 1) % 6;
		const CvAStarNode* rl = node->m_apNeighbors[eRearLeft];
		const CvAStarNode* rr = node->m_apNeighbors[eRearRight];

		if (!rl || !StepValidGeneric(parent,rl,0,data,finder,bAnyArea,false))
			return false;
		if (!rr || !StepValidGeneric(parent,rr,0,data,finder,bAnyArea,false))
			return false;
	}

	return TRUE;
}

int StepValid(const CvAStarNode* parent, const CvAStarNode* node, int, const SPathFinderUserData& data, const CvAStar* finder)
{
	return StepValidGeneric(parent,node,0,data,finder,false,false);
}
int StepValidAnyArea(const CvAStarNode* parent, const CvAStarNode* node, int, const SPathFinderUserData& data, const CvAStar* finder)
{
	return StepValidGeneric(parent,node,0,data,finder,true,false);
}
int StepValidWide(const CvAStarNode* parent, const CvAStarNode* node, int, const SPathFinderUserData& data, const CvAStar* finder)
{
	return StepValidGeneric(parent,node,0,data,finder,false,true);
}
int StepValidWideAnyArea(const CvAStarNode* parent, const CvAStarNode* node, int, const SPathFinderUserData& data, const CvAStar* finder)
{
	return StepValidGeneric(parent,node,0,data,finder,true,true);
}

//	--------------------------------------------------------------------------------
/// Step path finder - add a new path
int StepAdd(CvAStarNode* parent, CvAStarNode* node, int operation, const SPathFinderUserData&, CvAStar*)
{
	if(operation == ASNC_INITIALADD)
	{
		node->m_iTurns = 0;
	}
	else
	{
		node->m_iTurns = (parent->m_iTurns + 1);
	}

	node->m_iMoves = 0;
	return 1;
}

//	--------------------------------------------------------------------------------
/// Influence path finder - is this end point for the path valid?
int InfluenceDestValid(int iToX, int iToY, const SPathFinderUserData& data, const CvAStar* finder)
{
	CvMap& kMap = GC.getMap();
	CvPlot* pFromPlot = kMap.plotUnchecked(finder->GetStartX(), finder->GetStartY());
	CvPlot* pToPlot = kMap.plotUnchecked(iToX, iToY);

	if(plotDistance(pFromPlot->getX(),pFromPlot->getY(),pToPlot->getX(),pToPlot->getY()) > data.iTypeParameter)
		return FALSE;

	return TRUE;
}


//	--------------------------------------------------------------------------------
/// Influence path finder - determine heuristic cost
int InfluenceHeuristic(int iFromX, int iFromY, int iToX, int iToY)
{
	return plotDistance(iFromX, iFromY, iToX, iToY);
}

//	--------------------------------------------------------------------------------
/// Influence path finder - compute cost of a path
int InfluenceCost(const CvAStarNode* parent, CvAStarNode* node, int, const SPathFinderUserData& data, const CvAStar* finder)
{
	int iCost = 0;
	bool bDifferentOwner = false;

	if(parent->m_pParent || GC.getUSE_FIRST_RING_INFLUENCE_TERRAIN_COST())
	{
		CvMap& kMap = GC.getMap();
		CvPlot* pFromPlot = kMap.plotUnchecked(parent->m_iX, parent->m_iY);
		CvPlot* pToPlot = kMap.plotUnchecked(node->m_iX, node->m_iY);
		CvPlot* pSourcePlot = kMap.plotUnchecked(finder->GetStartX(), finder->GetStartY());

		int iRange = data.iTypeParameter;
		if(iRange >= 0)
		{
			if(pToPlot->getOwner() != NO_PLAYER && pSourcePlot->getOwner() != NO_PLAYER && pToPlot->getOwner() != pSourcePlot->getOwner())
				bDifferentOwner = true;
		}

		if(pFromPlot->isRiverCrossing(directionXY(pFromPlot, pToPlot)))
			iCost += GC.getINFLUENCE_RIVER_COST();

		// Mountain Cost
		if(pToPlot->isMountain())
			iCost += GC.getINFLUENCE_MOUNTAIN_COST();
		// Not a mountain - use the terrain cost
		else
		{
			// Hill cost
			if(pToPlot->isHills())
				iCost += GC.getINFLUENCE_HILL_COST();
			iCost += GC.getTerrainInfo(pToPlot->getTerrainType())->getInfluenceCost();
			iCost += ((pToPlot->getFeatureType() == NO_FEATURE) ? 0 : GC.getFeatureInfo(pToPlot->getFeatureType())->getInfluenceCost());
		}
	}
	else
	{
		iCost = 1;
	}

	iCost = std::max(1,iCost);
	iCost = std::min(3,iCost);
	if (bDifferentOwner)
	{
		iCost += 15;
	}

	return iCost;
}


//	--------------------------------------------------------------------------------
/// Influence path finder - check validity of a coordinate
int InfluenceValid(const CvAStarNode* parent, const CvAStarNode* node, int, const SPathFinderUserData& data, const CvAStar*)
{
	if(parent == NULL)
		return TRUE;

	CvPlot* pFromPlot = GC.getMap().plotUnchecked(parent->m_iX, parent->m_iY);
	CvPlot* pToPlot = GC.getMap().plotUnchecked(node->m_iX, node->m_iY);

	if (!pFromPlot || !pToPlot)
		return FALSE;

	if(plotDistance(pFromPlot->getX(),pFromPlot->getY(),pToPlot->getX(),pToPlot->getY()) > data.iTypeParameter)
		return FALSE;

	return TRUE;
}

//	--------------------------------------------------------------------------------
// Route - Return the x, y plot of the node that we want to access
int RouteGetExtraChild(const CvAStarNode* node, int iIndex, int& iX, int& iY, const CvAStar* finder)
{
	iX = -1;
	iY = -1;

	PlayerTypes ePlayer = finder->GetData().ePlayer;
	CvPlayerAI& kPlayer = GET_PLAYER(ePlayer);
	TeamTypes eTeam = kPlayer.getTeam();
	CvPlot* pPlot = GC.getMap().plotCheckInvalid(node->m_iX, node->m_iY);

	if(!pPlot)
	{
		return 0;
	}

	CvCity* pCity = pPlot->getPlotCity();

	// if there isn't a city there or the city isn't on our team
	if(!pCity || pCity->getTeam() != eTeam)
	{
		return 0;
	}

	int iValidCount = 0;
	CvCityConnections* pCityConnections = kPlayer.GetCityConnections();

	uint uiFirstCityIndex = pCityConnections->GetIndexFromCity(pCity);
	for(uint uiSecondCityIndex = 0; uiSecondCityIndex < pCityConnections->m_aiCityPlotIDs.size(); uiSecondCityIndex++)
	{
		if(uiFirstCityIndex == uiSecondCityIndex)
		{
			continue;
		}

		CvCityConnections::RouteInfo* pRouteInfo = pCityConnections->GetRouteInfo(uiFirstCityIndex, uiSecondCityIndex);
		if(!pRouteInfo)
		{
			continue;
		}

		// get the two cities
		CvCity* pFirstCity  = pCityConnections->GetCityFromIndex(uiFirstCityIndex);
		CvCity* pSecondCity = pCityConnections->GetCityFromIndex(uiSecondCityIndex);

		if(!pFirstCity || !pSecondCity)
		{
			continue;
		}

#if defined(MOD_EVENTS_CITY_CONNECTIONS)
		if(pRouteInfo->m_cRouteState & CvCityConnections::HAS_INDIRECT_ROUTE)
#else
		if(pRouteInfo->m_cRouteState & CvCityConnections::HAS_WATER_ROUTE)
#endif
		{
			if(iValidCount == iIndex)
			{
				iX = pSecondCity->getX();
				iY = pSecondCity->getY();
				return 1;
			}
			iValidCount++;
		}
	}

	return 0;
}

//	---------------------------------------------------------------------------
/// Route path finder - check validity of a coordinate
int RouteValid(const CvAStarNode* parent, const CvAStarNode* node, int, const SPathFinderUserData& data, const CvAStar*)
{
	if(parent == NULL || data.ePlayer==NO_PLAYER)
		return TRUE;

	PlayerTypes ePlayer = data.ePlayer;
	RouteTypes eRoute = (RouteTypes)data.iTypeParameter;

	CvPlot* pNewPlot = GC.getMap().plotUnchecked(node->m_iX, node->m_iY);
	CvPlayer& kPlayer = GET_PLAYER(ePlayer);

	RouteTypes ePlotRoute = pNewPlot->getRouteType();

	if(pNewPlot->IsRoutePillaged())
		ePlotRoute = NO_ROUTE;

	if (ePlotRoute==NO_ROUTE)
	{
		//what else can count as road depends on the player type
		if(kPlayer.GetPlayerTraits()->IsRiverTradeRoad())
		{
			if(pNewPlot->isRiver())
				ePlotRoute = ROUTE_ROAD;
		}
		if(kPlayer.GetPlayerTraits()->IsMoveFriendlyWoodsAsRoad())
		{
			if(pNewPlot->getOwner() == ePlayer)
				if(pNewPlot->getFeatureType() == FEATURE_FOREST || pNewPlot->getFeatureType() == FEATURE_JUNGLE)
					ePlotRoute = ROUTE_ROAD;
		}
	}

	if(!pNewPlot->IsFriendlyTerritory(ePlayer))
	{
		PlayerTypes ePlotOwnerPlayer = pNewPlot->getOwner();
		if(ePlotOwnerPlayer != NO_PLAYER)
		{
			PlayerTypes eMajorPlayer = NO_PLAYER;
			PlayerTypes eMinorPlayer = NO_PLAYER;
			CvPlayer& kPlotOwner = GET_PLAYER(ePlotOwnerPlayer);
			if(kPlayer.isMinorCiv() && !kPlotOwner.isMinorCiv())
			{
				eMajorPlayer = ePlotOwnerPlayer;
				eMinorPlayer = ePlayer;
			}
			else if(kPlotOwner.isMinorCiv() && !kPlayer.isMinorCiv())
			{
				eMajorPlayer = ePlayer;
				eMinorPlayer = ePlotOwnerPlayer;
			}
			else
			{
				return FALSE;
			}

			if(!GET_PLAYER(eMinorPlayer).GetMinorCivAI()->IsActiveQuestForPlayer(eMajorPlayer, MINOR_CIV_QUEST_ROUTE))
			{
				return FALSE;
			}
		}
	}

	//which route types are allowed?
	if ( eRoute == NO_ROUTE )
	{
#if defined(MOD_EVENTS_CITY_CONNECTIONS)
		// Cities always have the best route, which permits "harbour to harbour" connections before The Wheel
		if(pNewPlot->isCity())
		{
			return TRUE;
		}
#endif

		// if the player can't build
		if(kPlayer.getBestRoute() == NO_ROUTE)
		{
			return FALSE;
		}

		if(ePlotRoute != NO_ROUTE)
		{
			return TRUE;
		}
	}
	else
	{
		if(ePlotRoute == eRoute)
		{
			return TRUE;
		}
	}

	return FALSE;
}

//	---------------------------------------------------------------------------
// Route - find the number of additional children. In this case, the node is at a city, push all other cities that the city has a water connection to
// This function does not require the global Tactical Analysis Map.
int RouteGetNumExtraChildren(const CvAStarNode* node, const CvAStar* finder)
{
	PlayerTypes ePlayer = finder->GetData().ePlayer;
	CvPlayerAI& kPlayer = GET_PLAYER(ePlayer);
	TeamTypes eTeam = kPlayer.getTeam();
	CvPlot* pPlot = GC.getMap().plotCheckInvalid(node->m_iX, node->m_iY);

	if(!pPlot)
	{
		return 0;
	}

	// slewis - don't allow the minor civ to use harbors
	if(kPlayer.isMinorCiv())
	{
		return 0;
	}

	CvCityConnections* pCityConnections = kPlayer.GetCityConnections();
	if(pCityConnections->IsEmpty())
	{
		return 0;
	}

	int iResultNum = 0;

	CvCity* pCity = pPlot->getPlotCity();

	// if there isn't a city there or the city isn't on our team
	if(!pCity || pCity->getTeam() != eTeam)
	{
		return 0;
	}

	uint uiFirstCityIndex = pCityConnections->GetIndexFromCity(pCity);
	if(uiFirstCityIndex >= pCityConnections->m_aiCityPlotIDs.size())
	{
		CvAssertMsg(false, "City index out of bounds");
		return 0;
	}

	for(uint uiSecondCityIndex = 0; uiSecondCityIndex < pCityConnections->m_aiCityPlotIDs.size(); uiSecondCityIndex++)
	{
		if(uiFirstCityIndex == uiSecondCityIndex)
		{
			continue;
		}

		CvCityConnections::RouteInfo* pRouteInfo = pCityConnections->GetRouteInfo(uiFirstCityIndex, uiSecondCityIndex);
		if(!pRouteInfo)
		{
			continue;
		}

		// get the two cities
		CvCity* pFirstCity  = pCityConnections->GetCityFromIndex(uiFirstCityIndex);
		CvCity* pSecondCity = pCityConnections->GetCityFromIndex(uiSecondCityIndex);

		if(!pFirstCity || !pSecondCity)
		{
			continue;
		}

#if defined(MOD_EVENTS_CITY_CONNECTIONS)
		if(pRouteInfo->m_cRouteState & CvCityConnections::HAS_INDIRECT_ROUTE)
#else
		if(pRouteInfo->m_cRouteState & CvCityConnections::HAS_WATER_ROUTE)
#endif
		{
			iResultNum++;
		}
	}

	return iResultNum;
}

//	--------------------------------------------------------------------------------
/// Water route valid finder - check the validity of a coordinate
int WaterRouteValid(const CvAStarNode* parent, const CvAStarNode* node, int, const SPathFinderUserData& data, const CvAStar*)
{
	if(parent == NULL)
		return TRUE;

	PlayerTypes ePlayer = data.ePlayer;
	TeamTypes eTeam = GET_PLAYER(ePlayer).getTeam();

	CvPlot* pNewPlot = GC.getMap().plotUnchecked(node->m_iX, node->m_iY);

	if(!(pNewPlot->isRevealed(eTeam)))
		return FALSE;

	CvCity* pCity = pNewPlot->getPlotCity();
	if(pCity && pCity->getTeam() == eTeam)
		return TRUE;

	if(pNewPlot->isWater())
		return TRUE;

	return FALSE;
}

//	--------------------------------------------------------------------------------
/// Build route cost
int BuildRouteCost(const CvAStarNode* /*parent*/, CvAStarNode* node, int, const SPathFinderUserData& data, const CvAStar*)
{
	CvPlot* pPlot = GC.getMap().plotUnchecked(node->m_iX, node->m_iY);

	PlayerTypes ePlayer = data.ePlayer;
	RouteTypes eRoute = (RouteTypes)data.iTypeParameter;

	if(pPlot->getRouteType() != NO_ROUTE)
	{
		if(pPlot->getRouteType() == eRoute)
			return 1;
		else
			return PATH_BUILD_ROUTE_REUSE_EXISTING_WEIGHT;
	}

	int iMaxValue = PATH_BUILD_ROUTE_BASE_VALUE;

	// if the plot is on a removable feature, it tends to be a good idea to build a road here
	int iMovementCost = ((pPlot->getFeatureType() == NO_FEATURE) ? GC.getTerrainInfo(pPlot->getTerrainType())->getMovementCost() : GC.getFeatureInfo(pPlot->getFeatureType())->getMovementCost());

	// calculate the max value based on how much of a movement increase we get
	if(iMovementCost + 1 != 0)
	{
		iMaxValue = iMaxValue / 2 + iMaxValue / (iMovementCost + 1);
	}

	// if the tile already been tagged for building a road, then provide a discount
	if(pPlot->GetBuilderAIScratchPadTurn() == GC.getGame().getGameTurn() && pPlot->GetBuilderAIScratchPadPlayer() == ePlayer)
	{
		iMaxValue = (int)(iMaxValue * PATH_BUILD_ROUTE_ALREADY_FLAGGED_DISCOUNT);
	}

	return iMaxValue;
}

//	--------------------------------------------------------------------------------
/// Build Route path finder - check validity of a coordinate
int BuildRouteValid(const CvAStarNode* parent, const CvAStarNode* node, int, const SPathFinderUserData& data, const CvAStar*)
{
	CvPlot* pNewPlot;

	if(parent == NULL || data.ePlayer == NO_PLAYER)
		return TRUE;

	PlayerTypes ePlayer = data.ePlayer;
	CvPlayer& thisPlayer = GET_PLAYER(ePlayer);
	bool bThisPlayerIsMinor = thisPlayer.isMinorCiv();

	pNewPlot = GC.getMap().plotUnchecked(node->m_iX, node->m_iY);
	if(!bThisPlayerIsMinor && !(pNewPlot->isRevealed(thisPlayer.getTeam())))
	{
		return FALSE;
	}

	if(pNewPlot->isWater())
	{
		return FALSE;
	}

	if(!pNewPlot->isValidEndTurnPlot(ePlayer))
	{
		return FALSE;
	}

	PlayerTypes ePlotOwnerPlayer = pNewPlot->getOwner();
	if(ePlotOwnerPlayer != NO_PLAYER && !pNewPlot->IsFriendlyTerritory(ePlayer))
	{
		PlayerTypes eMajorPlayer = NO_PLAYER;
		PlayerTypes eMinorPlayer = NO_PLAYER;
		bool bPlotOwnerIsMinor = GET_PLAYER(ePlotOwnerPlayer).isMinorCiv();
		if(bThisPlayerIsMinor && !bPlotOwnerIsMinor)
		{
			eMajorPlayer = ePlotOwnerPlayer;
			eMinorPlayer = ePlayer;
		}
		else if(bPlotOwnerIsMinor && !bThisPlayerIsMinor)
		{
			eMajorPlayer = ePlayer;
			eMinorPlayer = ePlotOwnerPlayer;
		}
		else
		{
			return FALSE;
		}

		if(!GET_PLAYER(eMinorPlayer).GetMinorCivAI()->IsActiveQuestForPlayer(eMajorPlayer, MINOR_CIV_QUEST_ROUTE))
		{
			return FALSE;
		}
	}

	return TRUE;
}


//	--------------------------------------------------------------------------------
/// Area path finder - check validity of a coordinate
int AreaValid(const CvAStarNode* parent, const CvAStarNode* node, int, const SPathFinderUserData&, const CvAStar*)
{
	if(parent == NULL)
	{
		return TRUE;
	}

	CvMap& kMap = GC.getMap();

	//this is independent of any team!
	if(kMap.plotUnchecked(parent->m_iX, parent->m_iY)->isImpassable() != kMap.plotUnchecked(node->m_iX, node->m_iY)->isImpassable())
	{
		return FALSE;
	}

	return ((kMap.plotUnchecked(parent->m_iX, parent->m_iY)->isWater() == kMap.plotUnchecked(node->m_iX, node->m_iY)->isWater()) ? TRUE : FALSE);
}


//	--------------------------------------------------------------------------------
/// Area path finder - callback routine when node added to open/closed list
int JoinArea(CvAStarNode*, CvAStarNode* node, int operation, const SPathFinderUserData& data, CvAStar*)
{
	if(operation == ASNL_ADDCLOSED)
	{
		GC.getMap().plotUnchecked(node->m_iX, node->m_iY)->setArea(data.iTypeParameter);
	}

	return 1;
}


//	--------------------------------------------------------------------------------
/// Area path finder - check validity of a coordinate
int LandmassValid(const CvAStarNode* parent, const CvAStarNode* node, int, const SPathFinderUserData&, const CvAStar*)
{
	if(parent == NULL)
	{
		return TRUE;
	}

	CvMap& kMap = GC.getMap();
	return ((kMap.plotUnchecked(parent->m_iX, parent->m_iY)->isWater() == kMap.plotUnchecked(node->m_iX, node->m_iY)->isWater()) ? TRUE : FALSE);
}


//	--------------------------------------------------------------------------------
/// Area path finder - callback routine when node added to open/closed list
int JoinLandmass(CvAStarNode*, CvAStarNode* node, int operation, const SPathFinderUserData& data, CvAStar*)
{
	if(operation == ASNL_ADDCLOSED)
	{
		GC.getMap().plotUnchecked(node->m_iX, node->m_iY)->setLandmass(data.iTypeParameter);
	}

	return 1;
}


// DERIVED CLASSES (which have more convenient ways to access our various pathfinders)

//	--------------------------------------------------------------------------------
/// Constructor
CvTwoLayerPathFinder::CvTwoLayerPathFinder()
{
	CvAStar::CvAStar();
	m_ppaaPartialMoveNodes = NULL;

	//this is our default path type
	m_sData.ePathType = PT_UNIT_WITH_ZOC;
	SetFunctionPointers(PathDest, PathDestValid, PathHeuristic, PathCost, PathValid, PathAdd, PathNodeAdd, NULL, NULL, UnitPathInitialize, UnitPathUninitialize);

#if defined(MOD_BALANCE_CORE)
	//for debugging
	m_strName = "TwoLayerAStar";
#endif
}

//	--------------------------------------------------------------------------------
/// Destructor
CvTwoLayerPathFinder::~CvTwoLayerPathFinder()
{
	CvAStar::DeInit();

	DeInit();
}

//	--------------------------------------------------------------------------------
/// Allocate memory, zero variables
void CvTwoLayerPathFinder::Initialize(int iColumns, int iRows, bool bWrapX, bool bWrapY)
{
	DeInit();

	CvAStar::Initialize(iColumns, iRows, bWrapX, bWrapY);

	m_ppaaPartialMoveNodes = FNEW(CvAStarNode*[m_iColumns], c_eCiv5GameplayDLL, 0);
	for(int iI = 0; iI < m_iColumns; iI++)
	{
		m_ppaaPartialMoveNodes[iI] = FNEW(CvAStarNode[m_iRows], c_eCiv5GameplayDLL, 0);
		for(int iJ = 0; iJ < m_iRows; iJ++)
		{
			m_ppaaPartialMoveNodes[iI][iJ].m_iX = iI;
			m_ppaaPartialMoveNodes[iI][iJ].m_iY = iJ;
		}
	}

	//re-use the base layer neighbors here!
	CvAStarNode** apNeighbors = m_ppaaNeighbors;
	for(int iI = 0; iI < m_iColumns; iI++)
	{
		for(int iJ = 0; iJ < m_iRows; iJ++)
		{
			//neighbors have already been precalculated in base class
			m_ppaaPartialMoveNodes[iI][iJ].m_apNeighbors = apNeighbors;
			apNeighbors += 6;
		}
	}
};

//	--------------------------------------------------------------------------------
/// Frees allocated memory
void CvTwoLayerPathFinder::DeInit()
{
	CvAStar::DeInit();

	if(m_ppaaPartialMoveNodes != NULL)
	{
		for(int iI = 0; iI < m_iColumns; iI++)
		{
			SAFE_DELETE_ARRAY(m_ppaaPartialMoveNodes[iI]);
		}

		SAFE_DELETE_ARRAY(m_ppaaPartialMoveNodes);
	}
}

//	--------------------------------------------------------------------------------
/// Return a node from the second layer of A-star nodes (for the partial moves)
CvAStarNode* CvTwoLayerPathFinder::GetPartialMoveNode(int iCol, int iRow)
{
	return &(m_ppaaPartialMoveNodes[iCol][iRow]);
}

//	--------------------------------------------------------------------------------
/// can do only certain types of path here
bool CvTwoLayerPathFinder::Configure(PathType ePathType)
{
	switch(ePathType)
	{
	case PT_UNIT_WITH_ZOC:
		SetFunctionPointers(PathDest, PathDestValid, PathHeuristic, PathCost, PathValid, PathAdd, PathNodeAdd, NULL, NULL, UnitPathInitialize, UnitPathUninitialize);
		m_iBasicPlotCost = PATH_MOVEMENT_WEIGHT*GC.getMOVE_DENOMINATOR();
		break;
	case PT_UI_PLOT_MOVE_HIGHLIGHT:
		SetFunctionPointers(PathDest, NULL, PathHeuristic, PathCost, UIPathValid, UIPathAdd, PathNodeAdd, NULL, NULL, UnitPathInitialize, UnitPathUninitialize);
		m_iBasicPlotCost = PATH_MOVEMENT_WEIGHT*GC.getMOVE_DENOMINATOR();
		break;
	case PT_UI_PLOT_ATTACK_HIGHLIGHT:
		SetFunctionPointers(PathDest, NULL, PathHeuristic, PathCost, UIPathValid, UIAttackPathAdd, PathNodeAdd, NULL, NULL, UnitPathInitialize, UnitPathUninitialize);
		m_iBasicPlotCost = PATH_MOVEMENT_WEIGHT*GC.getMOVE_DENOMINATOR();
		break;
	case PT_UI_PATH_VISUALIZIATION:
		SetFunctionPointers(PathDest, PathDestValid, PathHeuristic, PathCost, PathValid, PathAdd, PathNodeAdd, NULL, NULL, UnitPathInitialize, UnitPathUninitialize);
		m_iBasicPlotCost = PATH_MOVEMENT_WEIGHT*GC.getMOVE_DENOMINATOR();
		break;
	default:
		//not implemented here
		return false;
	}

	m_sData.ePathType = ePathType;
	return true;
}

//////////////////////////////////////////////////////////////////////////
// CvPathFinder convenience functions
//////////////////////////////////////////////////////////////////////////
bool CvPathFinder::Configure(PathType ePathType)
{
	switch(ePathType)
	{
	case PT_GENERIC_SAME_AREA:
		SetFunctionPointers(PathDest, StepDestValid, StepHeuristic, StepCost, StepValid, StepAdd, NULL, NULL, NULL, NULL, NULL);
		m_iBasicPlotCost = 1;
		break;
	case PT_GENERIC_ANY_AREA:
		SetFunctionPointers(PathDest, StepDestValid, StepHeuristic, StepCost, StepValidAnyArea, StepAdd, NULL, NULL, NULL, NULL, NULL);
		m_iBasicPlotCost = 1;
		break;
	case PT_GENERIC_SAME_AREA_WIDE:
		SetFunctionPointers(PathDest, StepDestValid, StepHeuristic, StepCost, StepValidWide, StepAdd, NULL, NULL, NULL, NULL, NULL);
		m_iBasicPlotCost = 1;
		break;
	case PT_GENERIC_ANY_AREA_WIDE:
		SetFunctionPointers(PathDest, StepDestValid, StepHeuristic, StepCost, StepValidWideAnyArea, StepAdd, NULL, NULL, NULL, NULL, NULL);
		m_iBasicPlotCost = 1;
		break;
	case PT_UNIT_IGNORE_OTHERS:
		SetFunctionPointers(PathDest, IgnoreUnitsDestValid, PathHeuristic, IgnoreUnitsCost, IgnoreUnitsValid, PathAdd, NULL, NULL, NULL, UnitPathInitialize, UnitPathUninitialize);
		m_iBasicPlotCost = PATH_MOVEMENT_WEIGHT*GC.getMOVE_DENOMINATOR();
		break;
	case PT_TRADE_WATER:
		SetFunctionPointers(PathDest, NULL, TradeRouteHeuristic, TradeRouteWaterPathCost, TradeRouteWaterValid, StepAdd, NULL, NULL, NULL, TradePathInitialize, TradePathUninitialize);
		m_iBasicPlotCost = PATH_TRADE_BASE_COST;
		break;
	case PT_TRADE_LAND:
		SetFunctionPointers(PathDest, NULL, TradeRouteHeuristic, TradeRouteLandPathCost, TradeRouteLandValid, StepAdd, NULL, NULL, NULL, TradePathInitialize, TradePathUninitialize);
		m_iBasicPlotCost = PATH_TRADE_BASE_COST;
		break;
	case PT_BUILD_ROUTE:
		SetFunctionPointers(PathDest, NULL, NULL, BuildRouteCost, BuildRouteValid, NULL, NULL, NULL, NULL, NULL, NULL);
		m_iBasicPlotCost = PATH_BUILD_ROUTE_BASE_VALUE;
		break;
	case PT_AREA_CONNECTION:
		SetFunctionPointers(PathDest, NULL, NULL, NULL, AreaValid, NULL, JoinArea, NULL, NULL, NULL, NULL);
		m_iBasicPlotCost = 1;
		break;
	case PT_LANDMASS_CONNECTION:
		SetFunctionPointers(PathDest, NULL, NULL, NULL, LandmassValid, NULL, JoinLandmass, NULL, NULL, NULL, NULL);
		m_iBasicPlotCost = 1;
		break;
	case PT_CITY_INFLUENCE:
		SetFunctionPointers(PathDest, InfluenceDestValid, InfluenceHeuristic, InfluenceCost, InfluenceValid, StepAdd, NULL, NULL, NULL, NULL, NULL);
		m_iBasicPlotCost = 1;
		break;
	case PT_CITY_ROUTE_LAND:
		SetFunctionPointers(PathDest, NULL, NULL, NULL, RouteValid, NULL, NULL, NULL, NULL, NULL, NULL);
		m_iBasicPlotCost = 1;
		break;
	case PT_CITY_ROUTE_WATER:
		SetFunctionPointers(PathDest, NULL, NULL, NULL, WaterRouteValid, NULL, NULL, NULL, NULL, NULL, NULL);
		m_iBasicPlotCost = 1;
		break;
	case PT_CITY_ROUTE_MIXED:
		SetFunctionPointers(PathDest, NULL, NULL, NULL, RouteValid, NULL, NULL, RouteGetNumExtraChildren, RouteGetExtraChild, NULL, NULL);
		m_iBasicPlotCost = 1;
		break;
	case PT_AIR_REBASE:
		SetFunctionPointers(PathDest, NULL, NULL, NULL, RebaseValid, NULL, NULL, RebaseGetNumExtraChildren, RebaseGetExtraChild, NULL, NULL);
		m_iBasicPlotCost = 1;
		break;
	default:
		//not implemented here
		return false;
	}

	m_sData.ePathType = ePathType;
	return true;
}

//	--------------------------------------------------------------------------------
/// configure the pathfinder and do the magic
bool CvPathFinder::GeneratePath(int iXstart, int iYstart, int iXdest, int iYdest, const SPathFinderUserData& data)
{
	if (!Configure(data.ePathType))
		return false;

	return CvAStar::GeneratePathWithCurrentConfiguration(iXstart, iYstart, iXdest, iYdest, data);
}

//	--------------------------------------------------------------------------------
/// Check for existence of step path between two points
bool CvPathFinder::DoesPathExist(const CvPlot* pStartPlot, const CvPlot* pEndPlot, const SPathFinderUserData& data)
{
	if(pStartPlot == NULL || pEndPlot == NULL)
		return false;

	return GeneratePath(pStartPlot->getX(), pStartPlot->getY(), pEndPlot->getX(), pEndPlot->getY(), data);
}

//	--------------------------------------------------------------------------------
/// Get the plot X from the end of the step path
CvPlot* CvPathFinder::GetXPlotsFromEnd(int iPlotsFromEnd, bool bLeaveEnemyTerritory) const
{
	CvPlot* currentPlot = NULL;
	PlayerTypes eEnemy = (PlayerTypes)GetData().iTypeParameter;

	int iPathLen = GetPathLength();
	int iNumSteps = ::min(iPlotsFromEnd, iPathLen);
	if(iNumSteps != -1)
	{
		CvAStarNode* pNode = GetLastNode();

		if(pNode != NULL)
		{
			// Starting at the end, loop backwards the correct number of times
			for(int i = 0; i < iNumSteps; i++)
			{
				if(pNode->m_pParent != NULL)
				{
					// Move to the previous plot on the path
					pNode = pNode->m_pParent;
				}
			}

			CvMap& kMap = GC.getMap();
			currentPlot = kMap.plotUnchecked(pNode->m_iX, pNode->m_iY);

			// Was an enemy specified and we don't want this plot to be in enemy territory?
			if (eEnemy != NO_PLAYER && bLeaveEnemyTerritory && currentPlot)
			{
				// Loop until we leave enemy territory
				for (int i = 0; i < (iPathLen - iNumSteps) && currentPlot->getOwner() == eEnemy; i++)
				{
					if (pNode->m_pParent != NULL)
					{
						// Move to the previous plot on the path
						pNode = pNode->m_pParent;
					}
					else
					{
						break;
					}
				}

				currentPlot = kMap.plotUnchecked(pNode->m_iX, pNode->m_iY);
			}
		}
	}

	return currentPlot;
}

//	--------------------------------------------------------------------------------
/// Returns the last plot along the step path owned by a specific player
int CvPathFinder::CountPlotsOwnedByXInPath(PlayerTypes ePlayer) const
{
	int iCount = 0;
	CvAStarNode* pNode = GetLastNode();

	// Starting at the end, loop until we find a plot from this owner
	CvMap& kMap = GC.getMap();
	while(pNode != NULL)
	{
		CvPlot* currentPlot;
		currentPlot = kMap.plotUnchecked(pNode->m_iX, pNode->m_iY);

		// Check and see if this plot has the right owner
		if(currentPlot->getOwner() == ePlayer)
			iCount++;

		// Move to the previous plot on the path
		pNode = pNode->m_pParent;
	}

	return iCount;
}
//	--------------------------------------------------------------------------------
/// Returns the last plot along the step path owned by a specific player
int CvPathFinder::CountPlotsOwnedAnyoneInPath(PlayerTypes eExceptPlayer) const
{
	int iCount = 0;
	CvAStarNode* pNode = GetLastNode();

	CvMap& kMap = GC.getMap();
	while(pNode != NULL)
	{
		CvPlot* currentPlot;
		currentPlot = kMap.plotUnchecked(pNode->m_iX, pNode->m_iY);

		// Check and see if this plot has an owner that isn't us.
		if(currentPlot->getOwner() != eExceptPlayer && currentPlot->getOwner() != NO_PLAYER)
			iCount++;

		// Move to the previous plot on the path
		pNode = pNode->m_pParent;
	}

	return iCount;
}

//	--------------------------------------------------------------------------------
/// Retrieve first node of path
CvPlot* CvPathFinder::GetPathFirstPlot() const
{
	CvAStarNode* pNode = GetLastNode();
	if (!pNode)
		return NULL;

	CvMap& kMap = GC.getMap();
	if(pNode->m_pParent == NULL)
	{
		return kMap.plotUnchecked(pNode->m_iX, pNode->m_iY);
	}

	while(pNode != NULL)
	{
		if(pNode->m_pParent->m_pParent == NULL)
		{
			return kMap.plotUnchecked(pNode->m_iX, pNode->m_iY);
		}

		pNode = pNode->m_pParent;
	}

	FAssert(false);

	return NULL;
}

//	--------------------------------------------------------------------------------
/// Return the furthest plot we can get to this turn that is on the path
CvPlot* CvPathFinder::GetPathEndTurnPlot() const
{
	CvAStarNode* pNode = GetLastNode();
	if (!pNode)
		return NULL;

	CvMap& kMap = GC.getMap();
	if((pNode->m_pParent == NULL) || (pNode->m_iTurns == 1))
	{
		return kMap.plotUnchecked(pNode->m_iX, pNode->m_iY);
	}

	while(pNode->m_pParent != NULL)
	{
		if(pNode->m_pParent->m_iTurns == 1)
		{
			return kMap.plotUnchecked(pNode->m_pParent->m_iX, pNode->m_pParent->m_iY);
		}

		pNode = pNode->m_pParent;
	}

	FAssert(false);

	return NULL;
}

//	--------------------------------------------------------------------------------
/// Get the whole path
SPath CvPathFinder::GetPath() const
{
	SPath ret;
	ret.iNormalizedDistance = INT_MAX;
	ret.iTurnGenerated = GC.getGame().getGameTurn();
	ret.sConfig = m_sData;

	CvAStarNode* pNode = GetLastNode();
	if (!pNode)
	{
		return ret;
	}

	ret.iNormalizedDistance = pNode->m_iKnownCost / m_iBasicPlotCost;

	//walk backwards ...
	while(pNode != NULL)
	{
		ret.vPlots.push_back( std::make_pair(pNode->m_iX, pNode->m_iY) );
		pNode = pNode->m_pParent;
	}

	std::reverse(ret.vPlots.begin(),ret.vPlots.end());
	return ret;
}

//	--------------------------------------------------------------------------------
/// check if a stored path is still viable
bool CvPathFinder::VerifyPath(const SPath& path)
{
	if (path.vPlots.size()<2)
		return false;

	//set the right config
	if (m_sData.ePathType != path.sConfig.ePathType)
		if (!Configure(path.sConfig.ePathType))
			return false;

	int iKnownCost = 0;
	for (size_t i=1; i<path.vPlots.size(); i++)
	{
		CvAStarNode& current = m_ppaaNodes[ path.vPlots[i-1].first ][ path.vPlots[i-1].second ];
		CvAStarNode& next = m_ppaaNodes[ path.vPlots[i].first ][ path.vPlots[i].second ];

		if ( udFunc(udValid, &current, &next, 0, m_sData) )
		{
			iKnownCost += udFunc(udCost, &current, &next, 0, m_sData);
			if (iKnownCost > path.iNormalizedDistance*m_iBasicPlotCost)
				return false;
		}
		else
			return false;
	}

	return true;
}


//	--------------------------------------------------------------------------------
/// UI path finder - check validity of a coordinate
int UIPathValid(const CvAStarNode* parent, const CvAStarNode* node, int operation, const SPathFinderUserData& data, const CvAStar* finder)
{
	if (parent == NULL)
	{
		return TRUE;
	}

	if(node->m_iTurns > 2)
	{
		return FALSE;
	}

	CvPlot* pToPlot = GC.getMap().plotUnchecked(node->m_iX, node->m_iY);

	const UnitPathCacheData* pCacheData = reinterpret_cast<const UnitPathCacheData*>(finder->GetScratchBuffer());
	const CvUnit* pUnit = pCacheData->pUnit;

	if(!pToPlot->isRevealed(pUnit->getTeam()))
	{
		if(pUnit->getNoRevealMapCount() > 0)
		{
			return FALSE;
		}
	}

	if(pToPlot->isVisible(pUnit->getTeam()) && pToPlot->isVisibleEnemyUnit(pUnit))
	{
		if (!pUnit->canMoveInto(*pToPlot, CvUnit::MOVEFLAG_ATTACK))
			return FALSE;
	}

	if(pUnit->getDomainType() == DOMAIN_LAND)
	{
		int iGroupAreaID = pUnit->getArea();
		if(pToPlot->getArea() != iGroupAreaID)
		{
			if(!(pToPlot->isAdjacentToArea(iGroupAreaID)))
			{
				// antonjs: Added for Smoky Skies scenario. Allows move range to show correctly for airships,
				// which move over land and sea plots equally (canMoveAllTerrain)
				if (!pUnit->canMoveAllTerrain())
				{
#if defined(MOD_BUGFIX_HOVERING_PATHFINDER)
					if (!(pUnit->IsHoveringUnit() && (pToPlot->isShallowWater() || pToPlot->getFeatureType() == FEATURE_ICE))) {
						return FALSE;
					}
#else
					return FALSE;
#endif
				}
			}
		}
	}

	if(!pUnit->canEnterTerrain(*pToPlot, CvUnit::MOVEFLAG_ATTACK))
	{
		return FALSE;
	}

	if(!PathValid(parent,node,operation,data,finder))
	{
		return FALSE;
	}

	return TRUE;
}

//	--------------------------------------------------------------------------------
/// UI path finder - add a new path and send out a message
int UIPathAdd(CvAStarNode* parent, CvAStarNode* node, int operation, const SPathFinderUserData& data, CvAStar* finder)
{
	PathAdd(parent, node, operation, data, finder);
	if(node && node->m_iTurns < 2)
	{
		CvPlot* pPlot = GC.getMap().plot(node->m_iX, node->m_iY);

		if(pPlot)
		{
			auto_ptr<ICvPlot1> pDllPlot = GC.WrapPlotPointer(pPlot);
			GC.GetEngineUserInterface()->AddHexToUIRange(pDllPlot.get());
		}
	}

	return 1;
}

//	--------------------------------------------------------------------------------
int UIAttackPathAdd(CvAStarNode* parent, CvAStarNode* node, int operation, const SPathFinderUserData& data, CvAStar* finder)
{
	PathAdd(parent, node, operation, data, finder);
	if(node && node->m_iTurns < 2)
	{
		CvPlot* pPlot = GC.getMap().plot(node->m_iX, node->m_iY);

		auto_ptr<ICvUnit1> pDllUnit(GC.GetEngineUserInterface()->GetHeadSelectedUnit());
		CvUnit* pUnit = GC.UnwrapUnitPointer(pDllUnit.get());
		CvAssertMsg(pUnit, "pUnit should be a value");

		if(pUnit && pPlot)
		{
			if(pPlot->isVisible(pUnit->getTeam()) && (pPlot->isVisibleEnemyUnit(pUnit) || pPlot->isEnemyCity(*pUnit)))
			{
				auto_ptr<ICvPlot1> pDllPlot = GC.WrapPlotPointer(pPlot);
				GC.GetEngineUserInterface()->AddHexToUIRange(pDllPlot.get());
			}
		}
	}

	return 1;
}

//	---------------------------------------------------------------------------
int RebaseValid(const CvAStarNode* parent, const CvAStarNode* node, int, const SPathFinderUserData&, const CvAStar* finder)
{
	if(parent == NULL)
		return TRUE;

	CvPlot* pNewPlot = GC.getMap().plotUnchecked(node->m_iX, node->m_iY);
	const UnitPathCacheData* pCacheData = reinterpret_cast<const UnitPathCacheData*>(finder->GetScratchBuffer());
	const CvUnit* pUnit = pCacheData->pUnit;
	if (!pUnit)
		return FALSE;

	//distance
	if (plotDistance(node->m_iX, node->m_iY, parent->m_iX, parent->m_iY) > pUnit->GetRange())
		return FALSE;

	//capacity
	if (pNewPlot->isCity() && pNewPlot->getPlotCity()->getOwner()==pUnit->getOwner())
	{
		int iUnitsThere = pNewPlot->countNumAirUnits( GET_PLAYER(pUnit->getOwner()).getTeam() );
		if (iUnitsThere < pNewPlot->getPlotCity()->GetMaxAirUnits())
			return TRUE;
	}
	else
	{
		IDInfo* pUnitNode = pNewPlot->headUnitNode();

		// Loop through all units on this plot
		while(pUnitNode != NULL)
		{
			CvUnit* pLoopUnit = ::getUnit(*pUnitNode);
			pUnitNode = pNewPlot->nextUnitNode(pUnitNode);
			
			if (pUnit->canLoad(*(pLoopUnit->plot())))
				return TRUE;
		}
	}

	return FALSE;
}

//	---------------------------------------------------------------------------
int RebaseGetNumExtraChildren(const CvAStarNode* node, const CvAStar*)
{
	CvPlot* pPlot = GC.getMap().plotCheckInvalid(node->m_iX, node->m_iY);
	if(!pPlot)
		return 0;

	CvCity* pCity = pPlot->getPlotCity();
	if (!pCity)
		return 0;

	// if there is a city and the city is on our team
	std::vector<int> vNeighbors = pCity->GetClosestFriendlyNeighboringCities();
	std::vector<int> vAttachedUnits = pCity->GetAttachedUnits();

	return (int)vNeighbors.size()+vAttachedUnits.size();
}

//	---------------------------------------------------------------------------
int RebaseGetExtraChild(const CvAStarNode* node, int iIndex, int& iX, int& iY, const CvAStar* finder)
{
	iX = -1;
	iY = -1;

	CvPlayer& kPlayer = GET_PLAYER(finder->GetData().ePlayer);

	CvPlot* pPlot = GC.getMap().plotCheckInvalid(node->m_iX, node->m_iY);
	if(!pPlot || iIndex<0)
		return 0;

	CvCity* pCity = pPlot->getPlotCity();
	if (!pCity)
		return 0;

	// if there is a city and the city is on our team
	std::vector<int> vNeighbors = pCity->GetClosestFriendlyNeighboringCities();
	std::vector<int> vAttachedUnits = pCity->GetAttachedUnits();

	if ( (size_t)iIndex<vNeighbors.size())
	{
		CvCity* pSecondCity = kPlayer.getCity(vNeighbors[iIndex]);
		if (pSecondCity)
		{
			iX = pSecondCity->getX();
			iY = pSecondCity->getY();
			return 1;
		}
	}
	else if ( (size_t)iIndex<vNeighbors.size()+vAttachedUnits.size() )
	{
		CvUnit* pCarrier = kPlayer.getUnit(vAttachedUnits[iIndex-vNeighbors.size()]);
		if (pCarrier)
		{
			iX = pCarrier->plot()->getX();
			iY = pCarrier->plot()->getY();
			return 1;
		}
	}

	return 0;
}

// A structure holding some unit values that are invariant during a path plan operation
struct TradePathCacheData
{
	PlayerTypes m_ePlayer;
	TeamTypes m_eTeam;
	bool m_bCanCrossOcean:1;
	bool m_bCanCrossMountain:1;
	bool m_bIsRiverTradeRoad:1;
	bool m_bIsMoveFriendlyWoodsAsRoad:1;

	inline PlayerTypes GetPlayer() const { return m_ePlayer; }
	inline TeamTypes GetTeam() const { return m_eTeam; }
	inline bool CanCrossOcean() const { return m_bCanCrossOcean; }
	inline bool CanCrossMountain() const { return m_bCanCrossMountain; }
	inline bool IsRiverTradeRoad() const { return m_bIsRiverTradeRoad; }
	inline bool IsMoveFriendlyWoodsAsRoad() const { return m_bIsMoveFriendlyWoodsAsRoad; }
};

//	--------------------------------------------------------------------------------
void TradePathInitialize(const SPathFinderUserData& data, CvAStar* finder)
{
	TradePathCacheData* pCacheData = reinterpret_cast<TradePathCacheData*>(finder->GetScratchBufferDirty());

	CvPlayer& kPlayer = GET_PLAYER(data.ePlayer);

	pCacheData->m_ePlayer = data.ePlayer;
	pCacheData->m_eTeam = kPlayer.getTeam();
	pCacheData->m_bCanCrossOcean = kPlayer.CanCrossOcean();
	pCacheData->m_bCanCrossMountain = kPlayer.CanCrossMountain();

	CvPlayerTraits* pPlayerTraits = kPlayer.GetPlayerTraits();
	if (pPlayerTraits)
	{
		pCacheData->m_bIsRiverTradeRoad = pPlayerTraits->IsRiverTradeRoad();
		pCacheData->m_bIsMoveFriendlyWoodsAsRoad = pPlayerTraits->IsMoveFriendlyWoodsAsRoad();
	}
	else
	{
		pCacheData->m_bIsRiverTradeRoad = false;
		pCacheData->m_bIsMoveFriendlyWoodsAsRoad = false;
	}

}

//	--------------------------------------------------------------------------------
void TradePathUninitialize(const SPathFinderUserData&, CvAStar*)
{

}

//	--------------------------------------------------------------------------------
int TradeRouteHeuristic(int iFromX, int iFromY, int iToX, int iToY)
{
	return plotDistance(iFromX, iFromY, iToX, iToY) * PATH_TRADE_BASE_COST/3;
}

//	--------------------------------------------------------------------------------
int TradeRouteLandPathCost(const CvAStarNode* parent, CvAStarNode* node, int, const SPathFinderUserData&, const CvAStar* finder)
{
	CvMap& kMap = GC.getMap();
	int iFromPlotX = parent->m_iX;
	int iFromPlotY = parent->m_iY;
	CvPlot* pFromPlot = kMap.plotUnchecked(iFromPlotX, iFromPlotY);

	int iToPlotX = node->m_iX;
	int iToPlotY = node->m_iY;
	CvPlot* pToPlot = kMap.plotUnchecked(iToPlotX, iToPlotY);

	const TradePathCacheData* pCacheData = reinterpret_cast<const TradePathCacheData*>(finder->GetScratchBuffer());
	FeatureTypes eFeature = pToPlot->getFeatureType();

	int iCost = PATH_TRADE_BASE_COST;

	// super duper low costs for moving along routes - don't check for pillaging
	if (pFromPlot->getRouteType() != NO_ROUTE && pToPlot->getRouteType() != NO_ROUTE)
		iCost = iCost / 4;
	// low costs for moving along rivers
	else if (pFromPlot->isRiver() && pToPlot->isRiver() && !(pFromPlot->isRiverCrossing(directionXY(pFromPlot, pToPlot))))
		iCost = iCost / 2;
	// Iroquios ability
	else if ((eFeature == FEATURE_FOREST || eFeature == FEATURE_JUNGLE) && pCacheData->IsMoveFriendlyWoodsAsRoad())
		iCost = iCost / 2;

	//try to avoid these plots
	if (pToPlot->isRoughGround())
		iCost += PATH_TRADE_BASE_COST/2;

	//prefer oasis
	if (eFeature == FEATURE_OASIS)
		iCost -= PATH_TRADE_BASE_COST/4;
	
	TeamTypes eToPlotTeam = pToPlot->getTeam();
	if (pCacheData->GetTeam() != eToPlotTeam)
	{
		//try to stick to friendly territory
		if (pToPlot->getOwner()==NO_PLAYER || !GET_TEAM(pCacheData->GetTeam()).IsAllowsOpenBordersToTeam(eToPlotTeam))
			iCost += PATH_TRADE_BASE_COST/4;
	}

	// avoid enemy lands
	if (eToPlotTeam != NO_TEAM && GET_TEAM(pCacheData->GetTeam()).isAtWar(eToPlotTeam))
	{
		iCost += PATH_TRADE_BASE_COST*10;
	}

	FAssert(iCost != MAX_INT);
	FAssert(iCost > 0);

	return iCost;
}

//	--------------------------------------------------------------------------------
int TradeRouteLandValid(const CvAStarNode* parent, const CvAStarNode* node, int, const SPathFinderUserData&, const CvAStar* finder)
{
	if(parent == NULL)
		return TRUE;

	const TradePathCacheData* pCacheData = reinterpret_cast<const TradePathCacheData*>(finder->GetScratchBuffer());
	CvMap& kMap = GC.getMap();
	CvPlot* pToPlot = kMap.plotUnchecked(node->m_iX, node->m_iY);
	CvPlot* pFromPlot = kMap.plotUnchecked(parent->m_iX, parent->m_iY);

	if (pToPlot->isWater())
	{
		return FALSE;
	}

	if(pFromPlot->getArea() != pToPlot->getArea())
	{
		return FALSE;
	}

	if (pToPlot->getImprovementType()==(ImprovementTypes)GC.getBARBARIAN_CAMP_IMPROVEMENT())
	{
		return FALSE;
	}

	if(!pToPlot->isValidMovePlot( pCacheData->GetPlayer() ))
	{
		return FALSE;
	}

	return TRUE;
}

//	--------------------------------------------------------------------------------

int TradeRouteWaterPathCost(const CvAStarNode*, CvAStarNode* node, int, const SPathFinderUserData&, const CvAStar* finder)
{
	CvMap& kMap = GC.getMap();
	const TradePathCacheData* pCacheData = reinterpret_cast<const TradePathCacheData*>(finder->GetScratchBuffer());

	int iToPlotX = node->m_iX;
	int iToPlotY = node->m_iY;
	CvPlot* pToPlot = kMap.plotUnchecked(iToPlotX, iToPlotY);

	int iCost = PATH_TRADE_BASE_COST;

	// prefer the coastline (not identical with coastal water)
	if (pToPlot->isWater() && pToPlot->isAdjacentToLand_Cached())
		iCost -= PATH_TRADE_BASE_COST/3;

	// avoid enemy lands
	TeamTypes eToPlotTeam = pToPlot->getTeam();
	if (eToPlotTeam != NO_TEAM && GET_TEAM(pCacheData->GetTeam()).isAtWar(eToPlotTeam))
	{
		iCost += PATH_TRADE_BASE_COST*10;
	}

	//try to stick to friendly territory
	if (pCacheData->GetTeam() != eToPlotTeam)
	{
		if (pToPlot->getOwner()==NO_PLAYER || !GET_TEAM(pCacheData->GetTeam()).IsAllowsOpenBordersToTeam(eToPlotTeam))
			iCost += PATH_TRADE_BASE_COST/8;
	}

	return iCost;
}

//	--------------------------------------------------------------------------------
int TradeRouteWaterValid(const CvAStarNode* parent, const CvAStarNode* node, int, const SPathFinderUserData&, const CvAStar* finder)
{
	if(parent == NULL)
		return TRUE;

	const TradePathCacheData* pCacheData = reinterpret_cast<const TradePathCacheData*>(finder->GetScratchBuffer());

	CvMap& kMap = GC.getMap();
	CvPlot* pNewPlot = kMap.plotUnchecked(node->m_iX, node->m_iY);

	ImprovementTypes eImprovement = pNewPlot->getImprovementType();
	CvImprovementEntry* pkImprovementInfo = (eImprovement != NO_IMPROVEMENT) ? GC.getImprovementInfo(eImprovement) : NULL;
	bool bIsPassableImprovement = MOD_GLOBAL_PASSABLE_FORTS && pkImprovementInfo != NULL && pkImprovementInfo->IsMakesPassable();
	if(!pNewPlot->isCity() && !bIsPassableImprovement)
	{
		if (!pNewPlot->isWater())
		{
			return FALSE;
		}

		if (pNewPlot->getTerrainType() != (TerrainTypes) GC.getSHALLOW_WATER_TERRAIN())	// Quicker shallow water test since we know that the plot is water already
		{
			if (!pCacheData->CanCrossOcean())
			{
				return FALSE;
			}
		}

		CvPlot* pParentPlot = kMap.plotUnchecked(parent->m_iX, parent->m_iY);

		ImprovementTypes eImprovement = pParentPlot->getImprovementType();
		CvImprovementEntry* pkImprovementInfo = (eImprovement != NO_IMPROVEMENT) ? GC.getImprovementInfo(eImprovement) : NULL;
		bool bParentIsPassableImprovement = MOD_GLOBAL_PASSABLE_FORTS && pkImprovementInfo != NULL && pkImprovementInfo->IsMakesPassable();
		if(!pParentPlot->isCity() && !bParentIsPassableImprovement)
		{
			if(pParentPlot->getArea() != pNewPlot->getArea())
			{
				return FALSE;
			}
		}

		//traderoute may pass a plot whenever the owner may pass it
		if(!pNewPlot->isValidMovePlot( pCacheData->GetPlayer() ))
		{
			return FALSE;
		}

	}

	return TRUE;
}

//	--------------------------------------------------------------------------------
// Copy the supplied node and its parent nodes into an array of simpler path nodes for caching purposes.
// It is ok to pass in NULL, the resulting array will contain zero elements
void CopyPath(const CvAStarNode* pkEndNode, CvPathNodeArray& kPathArray)
{
	if(pkEndNode != NULL)
	{
		const CvAStarNode* pkNode = pkEndNode;

		// Count the number of nodes
		uint uiNodeCount = 1;

		while(pkNode->m_pParent != NULL)
		{
			++uiNodeCount;
			pkNode = pkNode->m_pParent;
		}

		kPathArray.setsize(uiNodeCount);

		pkNode = pkEndNode;
		kPathArray[0] = *pkNode;

		uint uiIndex = 1;
		while(pkNode->m_pParent != NULL)
		{
			pkNode = pkNode->m_pParent;
			kPathArray[uiIndex++] = *pkNode;
		}
	}
	else
		kPathArray.setsize(0);	// Setting the size to 0 rather than clearing so that the array data is not deleted.  Helps with memory thrashing.
}

//	---------------------------------------------------------------------------
const CvPathNode* CvPathNodeArray::GetTurnDest(int iTurn)
{
	for (uint i = size(); i--; )
	{
		const CvPathNode& kNode = at(i);
		if (i == 0)
		{
			// Last node, only return it if it is the desired turn
			if (kNode.m_iTurns == iTurn)
				return &kNode;
			return NULL;
		}
		else
		{
			// Is this node the correct turn and the next node is a turn after it?
			if (kNode.m_iTurns == iTurn && at(i-1).m_iTurns > iTurn)
				return &kNode;
		}
	}

	return NULL;
}

//	---------------------------------------------------------------------------
bool IsPlotConnectedToPlot(PlayerTypes ePlayer, CvPlot* pFromPlot, CvPlot* pToPlot, RouteTypes eRestrictRoute, bool bIgnoreHarbors)
{
	if (ePlayer==NO_PLAYER || pFromPlot==NULL || pToPlot==NULL)
		return false;

	SPathFinderUserData data(ePlayer, bIgnoreHarbors ? PT_CITY_ROUTE_MIXED : PT_CITY_ROUTE_MIXED, eRestrictRoute);

	return GC.GetStepFinder().GeneratePath(pFromPlot->getX(), pFromPlot->getY(), pToPlot->getX(), pToPlot->getY(), data);
}

//	---------------------------------------------------------------------------
//convenience constructor
SPathFinderUserData::SPathFinderUserData(const CvUnit* pUnit, int _iFlags, int _iMaxTurns)
{
	ePathType = PT_UNIT_WITH_ZOC;
	iFlags = _iFlags;
	iMaxTurns = _iMaxTurns;
	ePlayer = pUnit ? pUnit->getOwner() : NO_PLAYER;
	iUnitID = pUnit ? pUnit->GetID() : 0;
	iTypeParameter = -1; //typical invalid enum
	iMaxNormalizedDistance = INT_MAX;
}

//	---------------------------------------------------------------------------
//convenience constructor
SPathFinderUserData::SPathFinderUserData(PlayerTypes _ePlayer, PathType _ePathType, int _iTypeParameter, int _iMaxTurns)
{
	ePathType = _ePathType;
	iFlags = 0;
	ePlayer = _ePlayer;
	iUnitID = 0;
	iTypeParameter = _iTypeParameter;
	iMaxTurns = _iMaxTurns;
	iMaxNormalizedDistance = INT_MAX;
}