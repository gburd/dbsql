# This file implements some common TCL routines used for regression
# testing the DBSQL library

# Make sure the Tcl API was compiled such that we encode/decode strings
# passed between the two libraries properly.  Abort now with an error
# message if not.
#
if {[dbsql -tcl-uses-utf]} {
  if {"\u1234"=="u1234"} {
    puts stderr "***** BUILD PROBLEM *****"
    puts stderr "$argv0 was linked against an older version"
    puts stderr "of Tcl that does not support Unicode, but uses a header"
    puts stderr "file (\"tcl.h\") from a new Tcl version that does support"
    puts stderr "Unicode.  This combination causes internal errors."
    puts stderr "Recompile using a Tcl library and header file that match"
    puts stderr "and try again."
    puts stderr "**************************"
    exit 1
  }
} else {
  if {"\u1234"!="u1234"} {
    puts stderr "***** BUILD PROBLEM *****"
    puts stderr "$argv0 was linked against an newer version"
    puts stderr "of Tcl that supports Unicode, but uses a header file"
    puts stderr "(\"tcl.h\") from a old Tcl version that does not support"
    puts stderr "Unicode.  This combination causes internal errors."
    puts stderr "Recompile using a Tcl library and header file that match"
    puts stderr "and try again."
    puts stderr "**************************"
    exit 1
  }
}

# Create a test database.
#
catch {db close}
file delete -force test.db
file delete -force test.db-journal
dbsql db ./test.db
if {[info exists ::SETUP_SQL]} {
  db eval $::SETUP_SQL
}

# Abort early if this script has been run before.
#
if {[info exists nTest]} return

# Set the test counters to zero.
#
set nErr 0
set nTest 0
set nProb 0
set skip_test 0
set failList {}

# Invoke the do_test procedure to run a single test 
#
proc do_test {name cmd expected} {
  global argv nErr nTest skip_test
  if {$skip_test} {
    set skip_test 0
    return
  }
  if {[llength $argv]==0} { 
    set go 1
  } else {
    set go 0
    foreach pattern $argv {
      if {[string match $pattern $name]} {
        set go 1
        break
      }
    }
  }
  if {!$go} return
  incr nTest
  puts -nonewline $name...
  flush stdout
  if {[catch {uplevel #0 "$cmd;\n"} result]} {
    puts "\nError: $result"
    incr nErr
    lappend ::failList $name
    if {$nErr>10} {puts "*** Giving up..."; finalize_testing}
  } elseif {[string compare $result $expected]} {
    puts "\nExpected: \[$expected\]\n     Got: \[$result\]"
    incr nErr
    lappend ::failList $name
    if {$nErr>10} {puts "*** Giving up..."; finalize_testing}
  } else {
    puts " Ok"
  }
}

# Invoke this procedure on a test that is probabilistic
# and might fail sometimes.
#
proc do_probtest {name cmd expected} {
  global argv nProb nTest skip_test
  if {$skip_test} {
    set skip_test 0
    return
  }
  if {[llength $argv]==0} { 
    set go 1
  } else {
    set go 0
    foreach pattern $argv {
      if {[string match $pattern $name]} {
        set go 1
        break
      }
    }
  }
  if {!$go} return
  incr nTest
  puts -nonewline $name...
  flush stdout
  if {[catch {uplevel #0 "$cmd;\n"} result]} {
    puts "\nError: $result"
    incr nErr
  } elseif {[string compare $result $expected]} {
    puts "\nExpected: \[$expected\]\n     Got: \[$result\]"
    puts "NOTE: The results of the previous test depend on system load"
    puts "and processor speed.  The test may sometimes fail even if the"
    puts "library is working correctly."
    incr nProb	
  } else {
    puts " Ok"
  }
}

# The procedure uses the special "dbsql_malloc_stat" command
# (which is only available if Dbsql is compiled with -DMEMORY_DEBUG=1)
# to see how many malloc()s have not been free()ed.  The number
# of surplus malloc()s is stored in the global variable $::Leak.
# If the value in $::Leak grows, it may mean there is a memory leak
# in the library.
#
proc memleak_check {} {
  if {[info command dbsql_malloc_stat]!=""} {
    set r [dbsql_malloc_stat]
    set ::Leak [expr {[lindex $r 0]-[lindex $r 1]}]
  }
}

# Run this routine last
#
proc finish_test {} {
  finalize_testing
}
proc finalize_testing {} {
  global nTest nErr nProb dbsql_open_file_count
  if {$nErr==0} memleak_check
  catch {db close}
  puts "$nErr errors out of $nTest tests"
  puts "Failures on these tests: $::failList"
  if {$nProb>0} {
    puts "$nProb probabilistic tests also failed, but this does"
    puts "not necessarily indicate a malfunction."
  }
  if {$dbsql_open_file_count} {
    puts "$dbsql_open_file_count files were left open"
    incr nErr
  }
  exit [expr {$nErr>0}]
}

# A procedure to execute SQL
#
proc execsql {sql {db db}} {
  # puts "SQL = $sql"
  return [$db eval $sql]
}

# Execute SQL and catch exceptions.
#
proc catchsql {sql {db db}} {
  # puts "SQL = $sql"
  set r [catch {$db eval $sql} msg]
  lappend r $msg
  return $r
}

# Do an VDBE code dump on the SQL given
#
proc explain {sql {db db}} {
  puts ""
  puts "addr  opcode        p1       p2     p3             "
  puts "----  ------------  ------  ------  ---------------"
  $db eval "explain $sql" {} {
    puts [format {%-4d  %-12.12s  %-6d  %-6d  %s} $addr $opcode $p1 $p2 $p3]
  }
}

# Another procedure to execute SQL.  This one includes the field
# names in the returned list.
#
proc execsql2 {sql} {
  set result {}
  db eval $sql data {
    foreach f $data(*) {
      lappend result $f $data($f)
    }
  }
  return $result
}

# Use the non-callback API to execute multiple SQL statements
#
proc stepsql {dbptr sql} {
  set sql [string trim $sql]
  set r 0
  while {[string length $sql]>0} {
    if {[catch {dbsql_compile $dbptr $sql sqltail} vm]} {
      return [list 1 $vm]
    }
    set sql [string trim $sqltail]
    while {[dbsql_step $vm N VAL COL]=="DBSQL_ROW"} {
      foreach v $VAL {lappend r $v}
    }
    if {[catch {dbsql_close_sqlvm $vm} errmsg]} {
      return [list 1 $errmsg]
    }
  }
  return $r
}

# Delete a file or directory
#
proc forcedelete {filename} {
  if {[catch {file delete -force $filename}]} {
    exec rm -rf $filename
  }
}

# Do an integrity check of the entire database
#
proc integrity_check {name} {
  do_test $name {
    execsql {PRAGMA integrity_check}
  } {ok}
}
