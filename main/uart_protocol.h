#pragma once

/**
 * Shared UART line protocol — see docs/UART_PROTOCOL.md
 */

#define UART_LINE_MAX 128

#define UART_CMD_FORWARD      "CMD:F"
#define UART_CMD_BACKWARD     "CMD:B"
#define UART_CMD_LEFT         "CMD:L"
#define UART_CMD_RIGHT        "CMD:R"
#define UART_CMD_STOP         "CMD:S"
#define UART_CMD_LED_ON       "CMD:LED_ON"
#define UART_CMD_LED_OFF      "CMD:LED_OFF"
#define UART_CMD_ESTOP        "CMD:ESTOP"
#define UART_CMD_ESTOP_CLR    "CMD:ESTOP_CLR"
#define UART_CMD_ADJUST       "CMD:ADJUST"
#define UART_CMD_PING         "CMD:PING"

#define UART_PREFIX_DIST      "DIST:"
#define UART_PREFIX_IMU       "IMU:"
#define UART_PREFIX_STATUS    "STATUS:"
#define UART_PREFIX_ADJUST    "ADJUST:"
#define UART_PREFIX_ACK       "ACK:"
#define UART_PREFIX_HEART     "HEART:"
#define UART_MSG_HELLO        "HELLO"
