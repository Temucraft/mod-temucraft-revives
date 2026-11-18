-- mod-temucraft-revives: per-map battle resurrection rules (override the config's instance-type rules).
-- start = charges when the instance begins, charges every `minutes` minutes, max = cap (0 = none).
CREATE TABLE IF NOT EXISTS `mod_temucraft_battle_res` (
    `map`     INT UNSIGNED NOT NULL,
    `start`   INT UNSIGNED NOT NULL DEFAULT 1,
    `charges` INT UNSIGNED NOT NULL DEFAULT 1,
    `minutes` INT UNSIGNED NOT NULL DEFAULT 10,
    `max`     INT UNSIGNED NOT NULL DEFAULT 0,
    `comment` VARCHAR(255) NOT NULL DEFAULT '',
    PRIMARY KEY (`map`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- the revive spells
DELETE FROM `spell_script_names` WHERE `ScriptName` = 'spell_temucraft_revive';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(145728, 'spell_temucraft_revive'), (115178, 'spell_temucraft_revive'), (25841, 'spell_temucraft_revive'),
(51918, 'spell_temucraft_revive'), (24341, 'spell_temucraft_revive');

DELETE FROM `module_string` WHERE `module` = 'mod-temucraft-revives';
INSERT INTO `module_string` (`module`, `id`, `string`) VALUES
('mod-temucraft-revives', 1, 'No battle resurrection charges left.'),
('mod-temucraft-revives', 2, '|cff80c0ffBattle resurrection|r used by {}: {} left (next in {} min).'),
('mod-temucraft-revives', 3, 'Battle resurrection charges here: {} (next in {} min).'),
('mod-temucraft-revives', 4, 'Battle resurrection charges only count in dungeons and raids.'),
('mod-temucraft-revives', 5, 'Not while in combat.');
