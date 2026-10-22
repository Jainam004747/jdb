CREATE TABLE notes (id INTEGER PRIMARY KEY, body TEXT);
INSERT INTO notes (body) VALUES ('it''s here'), ('semi;colon'), ('  spaces  '), ('');
INSERT INTO notes (body) VALUES ('unicode: crème brûlée ☕');
SELECT id, body, LENGTH(body) FROM notes;
SELECT id FROM notes WHERE body = '';
SELECT body FROM notes ORDER BY body;
