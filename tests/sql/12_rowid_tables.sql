CREATE TABLE log (msg TEXT, level INTEGER);
INSERT INTO log VALUES ('start', 1), ('warn', 2), ('stop', 1);
SELECT rowid, msg FROM log WHERE rowid >= 2;
DELETE FROM log WHERE rowid = 2;
INSERT INTO log VALUES ('again', 3);
SELECT rowid, msg, level FROM log;
SELECT level, COUNT(*) FROM log GROUP BY level;
