#include "ul/crc16.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_check_value(void)
{
    const uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    TEST_ASSERT_EQUAL_HEX16(0x29B1, ul_crc16(digits, sizeof(digits)));
}

static void test_empty_input_gives_the_initial_value(void)
{
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, ul_crc16(NULL, 0));
}

static void test_update_continues_a_crc(void)
{
    const uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    const uint16_t head = ul_crc16(data, 3);
    TEST_ASSERT_EQUAL_HEX16(ul_crc16(data, sizeof(data)), ul_crc16_update(head, &data[3], sizeof(data) - 3u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_check_value);
    RUN_TEST(test_empty_input_gives_the_initial_value);
    RUN_TEST(test_update_continues_a_crc);
    return UNITY_END();
}
