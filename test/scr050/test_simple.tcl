# Simple test - test.db should be a directory
set db [dbsql mydb test.db]
puts "Database created: $db"
set result [$db eval {SELECT 1 as test}]
puts "Query result: $result"
$db close
