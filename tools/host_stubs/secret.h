#ifndef HOST_TEST_SECRET_H
#define HOST_TEST_SECRET_H
/* Public dummy fleet for native tests; never read or embed site credentials. */
#define FLEET_NUM_UNITS 4
#define FLEET_BMS_TABLE { "test0", 0, {0} }, { "test1", 1, {0} }, \
                        { "test2", 2, {0} }, { "test3", 3, {0} }
#endif
