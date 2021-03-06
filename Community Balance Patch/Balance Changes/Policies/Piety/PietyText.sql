-- Piety Opener
UPDATE Language_en_US
SET Text = '[COLOR_POSITIVE_TEXT]Piety[ENDCOLOR] capitalizes on your investment in religion and trade.[NEWLINE][NEWLINE][COLOR_POSITIVE_TEXT]Adopting Piety grants:[ENDCOLOR] [NEWLINE] [ICON_BULLET]+100% Pressure in nearby Cities without your majority [ICON_RELIGION] Religion. [NEWLINE] [ICON_BULLET] All Religions (or just your founded [ICON_RELIGION] Religion, if you have one) spread to owned Cities without a majority from 50% further away.[NEWLINE] [ICON_BULLET] Unlocks building the [COLOR_CYAN]University of Sankore[ENDCOLOR].[NEWLINE][NEWLINE][COLOR_POSITIVE_TEXT]Each Piety policy unlocked grants:[ENDCOLOR] [NEWLINE] [ICON_BULLET] +3% [ICON_GOLD] Gold in Cities with a Temple.[NEWLINE][NEWLINE][COLOR_POSITIVE_TEXT]Adopting all Policies in Piety grants:[ENDCOLOR] [NEWLINE] [ICON_BULLET] Holy Sites produce +4 [ICON_GOLD] Gold and +4 [ICON_FOOD] Food. [NEWLINE] [ICON_BULLET] Cities that follow your [ICON_RELIGION] Majority Religion generate +1 of every Yield.[NEWLINE] [ICON_BULLET] Grants access to Renaissance Era Policy Branches, ignoring Era requirement.[NEWLINE] [ICON_BULLET] Provides [COLOR_CYAN]1[ENDCOLOR] point (of [COLOR_CYAN]3[ENDCOLOR] required in total) towards unlocking access to Ideologies.[NEWLINE] [ICON_BULLET] Allows for the purchase of [ICON_GREAT_ARTIST] Great Artists with [ICON_PEACE] Faith starting in the Industrial Era.'
WHERE Tag = 'TXT_KEY_POLICY_BRANCH_PIETY_HELP' AND EXISTS (SELECT * FROM COMMUNITY WHERE Type='COMMUNITY_CORE_BALANCE_POLICIES' AND Value= 1 );

-- Organized Religion
UPDATE Language_en_US
SET Text = '[COLOR_POSITIVE_TEXT]Organized Religion[ENDCOLOR][NEWLINE] -25% [ICON_PEACE] Faith purchase costs. +1 [ICON_PEACE] Faith and +1 [ICON_CULTURE] Culture from Temples.'
WHERE Tag = 'TXT_KEY_POLICY_ORGANIZED_RELIGION_HELP' AND EXISTS (SELECT * FROM COMMUNITY WHERE Type='COMMUNITY_CORE_BALANCE_POLICIES' AND Value= 1 );

-- Mandate of Heaven (Now Syncretism)
UPDATE Language_en_US
SET Text = 'Syncretism'
WHERE Tag = 'TXT_KEY_POLICY_MANDATE_OF_HEAVEN' AND EXISTS (SELECT * FROM COMMUNITY WHERE Type='COMMUNITY_CORE_BALANCE_POLICIES' AND Value= 1 );

UPDATE Language_en_US
SET Text = '[COLOR_POSITIVE_TEXT]Syncretism[ENDCOLOR][NEWLINE] +2 [ICON_RESEARCH] Science and +2 [ICON_PRODUCTION] Production from Markets, Caravansaries, and Harbors in Cities that follow your Religion. Your Religion spreads 50% more quickly over [ICON_INTERNATIONAL_TRADE] Trade Routes.'
WHERE Tag = 'TXT_KEY_POLICY_MANDATE_OF_HEAVEN_HELP' AND EXISTS (SELECT * FROM COMMUNITY WHERE Type='COMMUNITY_CORE_BALANCE_POLICIES' AND Value= 1 );

UPDATE Language_en_US
SET Text = 'Syncretism is the combining of different, often contradictory beliefs, while blending practices of various schools of thought. Syncretism involves the merger and analogizing of several originally discrete traditions, especially in the theology and mythology of religion, thus asserting an underlying unity and allowing for an inclusive approach to other faiths.'
WHERE Tag = 'TXT_KEY_POLICY_MANDATEOFHEAVEN_TEXT' AND EXISTS (SELECT * FROM COMMUNITY WHERE Type='COMMUNITY_CORE_BALANCE_POLICIES' AND Value= 1 );

-- Theocracy (Now Divine Right)
UPDATE Language_en_US
SET Text = 'Divine Right'
WHERE Tag = 'TXT_KEY_POLICY_THEOCRACY' AND EXISTS (SELECT * FROM COMMUNITY WHERE Type='COMMUNITY_CORE_BALANCE_POLICIES' AND Value= 1 );

UPDATE Language_en_US
SET Text = '[COLOR_POSITIVE_TEXT]Divine Right[ENDCOLOR][NEWLINE]Border growth is doubled in Cities during [ICON_GOLDEN_AGE] Golden Ages and ''We Love the King'' Day. Temples generate +1 [ICON_HAPPINESS_1] Happiness.'
WHERE Tag = 'TXT_KEY_POLICY_THEOCRACY_HELP' AND EXISTS (SELECT * FROM COMMUNITY WHERE Type='COMMUNITY_CORE_BALANCE_POLICIES' AND Value= 1 );

UPDATE Language_en_US
SET Text = 'The divine right of kings, or divine-right theory of kingship, is a political and religious doctrine of royal and political legitimacy. It asserts that a monarch is subject to no earthly authority, deriving the right to rule directly from the will of God. The king is thus not subject to the will of his people, the aristocracy, or any other estate of the realm, including (in the view of some, especially in Protestant countries) the Church. According to this doctrine, only God can judge an unjust king. The doctrine implies that any attempt to depose the king or to restrict his powers runs contrary to the will of God and may constitute a sacrilegious act. It is often expressed in the phrase "by the Grace of God," attached to the titles of a reigning monarch.'
WHERE Tag = 'TXT_KEY_POLICY_THEOCRACY_TEXT' AND EXISTS (SELECT * FROM COMMUNITY WHERE Type='COMMUNITY_CORE_BALANCE_POLICIES' AND Value= 1 );

-- Reformation (Now Monasticism)
UPDATE Language_en_US
SET Text = 'Monasticism'
WHERE Tag = 'TXT_KEY_POLICY_REFORMATION' AND EXISTS (SELECT * FROM COMMUNITY WHERE Type='COMMUNITY_CORE_BALANCE_POLICIES' AND Value= 1 );

UPDATE Language_en_US
SET Text = '[COLOR_POSITIVE_TEXT]Monasticism[ENDCOLOR][NEWLINE] Can purchase Monasteries with [ICON_PEACE] Faith (+2 [ICON_PEACE] Faith, +2 [ICON_FOOD] Food, +2 [ICON_RESEARCH] Science, 1 Scientist slot).'
WHERE Tag = 'TXT_KEY_POLICY_REFORMATION_HELP' AND EXISTS (SELECT * FROM COMMUNITY WHERE Type='COMMUNITY_CORE_BALANCE_POLICIES' AND Value= 1 );

UPDATE Language_en_US
SET Text = 'Monasticism or monkhood is a religious way of life in which one renounces worldly pursuits to devote oneself fully to spiritual work. Monastic life plays an important role in many Christian churches, especially in the Catholic and Orthodox traditions. Similar forms of religious life also exist in other faiths, most notably in Buddhism, but also in Hinduism and Jainism, although the expressions differ considerably.'
WHERE Tag = 'TXT_KEY_POLICY_REFORMATION_TEXT' AND EXISTS (SELECT * FROM COMMUNITY WHERE Type='COMMUNITY_CORE_BALANCE_POLICIES' AND Value= 1 );

-- Free Religion (Now Tolerance)
UPDATE Language_en_US
SET Text = 'Iconography'
WHERE Tag = 'TXT_KEY_POLICY_FREE_RELIGION' AND EXISTS (SELECT * FROM COMMUNITY WHERE Type='COMMUNITY_CORE_BALANCE_POLICIES' AND Value= 1 );

UPDATE Language_en_US
SET Text = '[COLOR_POSITIVE_TEXT]Iconography[ENDCOLOR][NEWLINE]All Specialists produce +1 [ICON_PEACE] Faith. A [ICON_GREAT_ARTIST] Great Artist appears near your [ICON_CAPITAL] Capital.'
WHERE Tag = 'TXT_KEY_POLICY_FREE_RELIGION_HELP' AND EXISTS (SELECT * FROM COMMUNITY WHERE Type='COMMUNITY_CORE_BALANCE_POLICIES' AND Value= 1 );

UPDATE Language_en_US
SET Text = 'Iconography is the branch of art history which studies the identification, description, and the interpretation of the content of images: the subjects depicted, the particular compositions and details used to do so, and other elements that are distinct from artistic style. One meaning of Iconography (based on a non-standard translation of the Greek and Russian equivalent terms) is the production of religious images, called icons, in the Byzantine and Orthodox Christian tradition; that is covered at Icon. In art history, "an iconography" may also mean a particular depiction of a subject in terms of the content of the image, such as the number of figures used, their placing and gestures. The term is also used in many academic fields other than art history, for example semiotics and media studies, and in general usage, for the content of images, the typical depiction in images of a subject, and related senses. Sometimes distinctions have been made between Iconology and Iconography, although the definitions, and so the distinction made, varies. When referring to movies, genres are immediately recognizable through their iconography, motifs that become associated with a specific genre through repetition.'
WHERE Tag = 'TXT_KEY_POLICY_FREERELIGION_TEXT' AND EXISTS (SELECT * FROM COMMUNITY WHERE Type='COMMUNITY_CORE_BALANCE_POLICIES' AND Value= 1 );

-- Monastery
UPDATE Language_en_US
SET Text = '+2 [ICON_FOOD] Food, +2 [ICON_RESEARCH] Science, +2 [ICON_PEACE] Faith, and 1 Scientist slot.'
WHERE Tag = 'TXT_KEY_BUILDING_MONASTERY_HELP' AND EXISTS (SELECT * FROM COMMUNITY WHERE Type='COMMUNITY_CORE_BALANCE_POLICIES' AND Value= 1 );

UPDATE Language_en_US
SET Text = 'Monasteries increase [ICON_PEACE] Faith, [ICON_FOOD] Food, and [ICON_RESEARCH] Science output, especially if you can afford to use the Scientist specialist. Can only be built if you have unlocked the Monasticism Policy in the Piety branch. Construct this building by purchasing it with [ICON_PEACE] Faith.'
WHERE Tag = 'TXT_KEY_BUILDING_MONASTERY_STRATEGY' AND EXISTS (SELECT * FROM COMMUNITY WHERE Type='COMMUNITY_CORE_BALANCE_POLICIES' AND Value= 1 );

