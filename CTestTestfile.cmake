# CMake generated Testfile for 
# Source directory: C:/Users/nirwa
# Build directory: C:/Users/nirwa/chronobook
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test(test_all "C:/Users/nirwa/chronobook/Debug/test_all.exe")
  set_tests_properties(test_all PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/nirwa/CMakeLists.txt;89;add_test;C:/Users/nirwa/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test(test_all "C:/Users/nirwa/chronobook/Release/test_all.exe")
  set_tests_properties(test_all PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/nirwa/CMakeLists.txt;89;add_test;C:/Users/nirwa/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test(test_all "C:/Users/nirwa/chronobook/MinSizeRel/test_all.exe")
  set_tests_properties(test_all PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/nirwa/CMakeLists.txt;89;add_test;C:/Users/nirwa/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test(test_all "C:/Users/nirwa/chronobook/RelWithDebInfo/test_all.exe")
  set_tests_properties(test_all PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/nirwa/CMakeLists.txt;89;add_test;C:/Users/nirwa/CMakeLists.txt;0;")
else()
  add_test(test_all NOT_AVAILABLE)
endif()
