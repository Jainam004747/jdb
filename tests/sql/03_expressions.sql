SELECT 1 + 2 * 3, (1 + 2) * 3, 7 / 2, -7 / 2, 7 % 3, -7 % 3;
SELECT 'jdb' || ' ' || 'rocks', 'n=' || 42, NULL || 'x';
SELECT 5 / 0, 5 % 0, NULL + 1, NULL = NULL, NULL IS NULL;
SELECT 1 AND 0, 1 OR 0, NULL AND 0, NULL OR 1, NULL AND 1, NOT 0, NOT 5;
SELECT LENGTH('hello'), UPPER('MiXeD'), LOWER('MiXeD'), ABS(-12), TYPEOF(1), TYPEOF('a'), TYPEOF(NULL);
SELECT 3 > 2, 2 >= 3, 'abc' < 'abd', 'b' > 'abc', 10 <> 10, 10 == 10;
SELECT -(-5), - 3 + 10, 2 * -3;
