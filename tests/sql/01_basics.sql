-- create, insert, select
CREATE TABLE menu (id INTEGER PRIMARY KEY, name TEXT NOT NULL, price INTEGER, category TEXT);
INSERT INTO menu (name, price, category) VALUES ('coffee', 199, 'drink'), ('tea', 159, 'drink');
INSERT INTO menu (name, price, category) VALUES ('donut', 129, 'food'), ('bagel', 249, 'food');
INSERT INTO menu VALUES (10, 'latte', 449, 'drink');
INSERT INTO menu (name) VALUES ('water');
SELECT * FROM menu;
SELECT name FROM menu WHERE price IS NULL;
SELECT id, name FROM menu WHERE id > 3;
SELECT COUNT(*) FROM menu;
