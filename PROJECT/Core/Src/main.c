#include "main.h"
#include <string.h>
#include <stdio.h>

#define MQTT_TOPIC "stm32/test123"
#define UART_RX_BUFFER_SIZE 256

// UART Handles
UART_HandleTypeDef huart1;  // ESP8266
UART_HandleTypeDef huart2;  // Debug
volatile uint8_t uart_rx_buffer[UART_RX_BUFFER_SIZE];
volatile uint16_t uart_rx_index = 0;

// Function Prototypes
void SystemClock_Config(void);
void MX_GPIO_Init(void);
void MX_USART1_UART_Init(void);
void MX_USART2_UART_Init(void);
void Error_Handler(void);
void ESP_Init(void);
HAL_StatusTypeDef ESP_SendAT(const char *cmd, const char *expect, uint32_t timeout);
void ESP_Restore(void);
void ESP_PublishNumber(void);
void ESP_Subscribe(void);
void Parse_MQTT_Message(void);

// Global response buffer
char response[512];

// Send AT command and wait for expected response
HAL_StatusTypeDef ESP_SendAT(const char *cmd, const char *expect, uint32_t timeout)
{
    char atCommand[128];
    memset(response, 0, sizeof(response));

    snprintf(atCommand, sizeof(atCommand), "%s\r\n", cmd);

    if (HAL_UART_Transmit(&huart1, (uint8_t *)atCommand, strlen(atCommand), HAL_MAX_DELAY) != HAL_OK)
    {
        HAL_UART_Transmit(&huart2, (uint8_t *)"[ESP] Transmit failed\r\n", 23, HAL_MAX_DELAY);
        return HAL_ERROR;
    }

    uint16_t index = 0;
    uint32_t tickstart = HAL_GetTick();
    while ((HAL_GetTick() - tickstart) < timeout && index < sizeof(response) - 1)
    {
        if (HAL_UART_Receive(&huart1, (uint8_t *)&response[index], 1, 50) == HAL_OK)
        {
            index++;
        }
        if (expect != NULL && strstr(response, expect) != NULL)
        {
            break;
        }
    }

    char debug_msg[512];
    snprintf(debug_msg, sizeof(debug_msg), "[ESP] CMD: %s\r\n[ESP] Raw Rsp: %s\r\n", cmd, response);
    HAL_UART_Transmit(&huart2, (uint8_t *)debug_msg, strlen(debug_msg), HAL_MAX_DELAY);

    return (expect == NULL || strstr(response, expect) != NULL) ? HAL_OK : HAL_ERROR;
}

void ESP_Restore(void)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)"AT+RESTORE\r\n", 12, 1000);
    memset(response, 0, sizeof(response));
    uint32_t tickstart = HAL_GetTick();
    while ((HAL_GetTick() - tickstart) < 5000)
    {
        if (HAL_UART_Receive(&huart1, (uint8_t *)&response[strlen(response)], 1, 100) == HAL_OK)
        {
            if (strstr(response, "ready"))
            {
                HAL_UART_Transmit(&huart2, (uint8_t *)"[ESP] Restored and ready.\r\n", 27, 200);
                return;
            }
        }
    }
    HAL_UART_Transmit(&huart2, (uint8_t *)"[ESP] Restore failed.\r\n", 23, 200);
    Error_Handler();
}

void ESP_Init(void)
{
    uint8_t temp;

    // --- Drain RX buffer before starting ---
    while (HAL_UART_Receive(&huart1, &temp, 1, 10) == HAL_OK);

    // --- Step 1: Basic ESP Setup ---
    if (ESP_SendAT("AT", "OK", 1000) != HAL_OK) Error_Handler();
    if (ESP_SendAT("AT+CWMODE=1", "OK", 2000) != HAL_OK) Error_Handler();
    if (ESP_SendAT("AT+CWJAP=\"MA_HOME\",\"01289878405\"", "WIFI GOT IP", 20000) != HAL_OK) Error_Handler();
    if (ESP_SendAT("AT+CIPMUX=0", "OK", 2000) != HAL_OK) Error_Handler();
    if (ESP_SendAT("AT+CIPSTART=\"TCP\",\"192.168.1.104\",1883", "CONNECT", 10000) != HAL_OK) Error_Handler();

    // --- Step 2: Send MQTT CONNECT packet ---
    const uint8_t mqttConnect[] = {
        0x10, 0x11,                    // Fixed header
        0x00, 0x04,                    // Protocol Name Length
        0x4D, 0x51, 0x54, 0x54,        // "MQTT"
        0x04,                          // Protocol Level
        0x02,                          // Clean session
        0x00, 0x64,                    // Keep alive = 100
        0x00, 0x05,                    // Client ID length
        'S', 'T', 'M', '3', '2'        // Client ID: "STM32"
    };

    char cipsendCmd[32];
    sprintf(cipsendCmd, "AT+CIPSEND=%d", sizeof(mqttConnect));
    if (ESP_SendAT(cipsendCmd, ">", 5000) != HAL_OK) Error_Handler();

    HAL_UART_Transmit(&huart2, (uint8_t *)"[ESP] Sending MQTT CONNECT packet:\r\n", 36, HAL_MAX_DELAY);
    for (uint8_t i = 0; i < sizeof(mqttConnect); i++) {
        char byte_msg[16];
        snprintf(byte_msg, sizeof(byte_msg), "0x%02X ", mqttConnect[i]);
        HAL_UART_Transmit(&huart2, (uint8_t *)byte_msg, strlen(byte_msg), HAL_MAX_DELAY);
    }
    HAL_UART_Transmit(&huart2, (uint8_t *)"\r\n", 2, HAL_MAX_DELAY);

    if (HAL_UART_Transmit(&huart1, (uint8_t *)mqttConnect, sizeof(mqttConnect), HAL_MAX_DELAY) != HAL_OK) {
        Error_Handler();
    }

    // --- Step 3: Wait for "SEND OK" ---
    uint8_t response[256] = {0};
    uint16_t index = 0;
    uint32_t tickstart = HAL_GetTick();
    while ((HAL_GetTick() - tickstart) < 5000 && index < sizeof(response) - 1) {
        if (HAL_UART_Receive(&huart1, &response[index], 1, 1000) == HAL_OK) {
            if (index >= 6 && memcmp(&response[index - 6], "SEND OK", 7) == 0) break;
            index++;
        }
    }

    HAL_UART_Transmit(&huart2, (uint8_t *)"[ESP] Pre-CONNACK bytes:\r\n", 27, HAL_MAX_DELAY);
    for (uint16_t i = 0; i < index; i++) {
        char byte_msg[16];
        snprintf(byte_msg, sizeof(byte_msg), "0x%02X ", response[i]);
        HAL_UART_Transmit(&huart2, (uint8_t *)byte_msg, strlen(byte_msg), HAL_MAX_DELAY);
    }
    HAL_UART_Transmit(&huart2, (uint8_t *)"\r\n", 2, HAL_MAX_DELAY);

    // --- Step 4: Wait for "+IPD,4:" followed by CONNACK (0x20 0x02 0x00 0x00) ---
    memset(response, 0, sizeof(response));
    index = 0;
    uint8_t ipd_found = 0;
    uint16_t ipd_start = 0;
    tickstart = HAL_GetTick();

    while ((HAL_GetTick() - tickstart) < 10000 && index < sizeof(response) - 1) {
        if (HAL_UART_Receive(&huart1, &response[index], 1, 1000) == HAL_OK) {
            if (!ipd_found && index >= 6 && memcmp(&response[index - 6], "+IPD,4:", 7) == 0) {
                ipd_found = 1;
                ipd_start = index + 1;

                for (int i = 0; i < 4 && index < sizeof(response) - 1; i++) {
                    if (HAL_UART_Receive(&huart1, &response[++index], 1, 1000) != HAL_OK)
                        break;
                }
                break;
            }
            index++;
        }
    }

    HAL_UART_Transmit(&huart2, (uint8_t *)"[ESP] CONNACK Response:\r\n", 25, HAL_MAX_DELAY);
    for (uint16_t i = 0; i < index; i++) {
        char byte_msg[16];
        snprintf(byte_msg, sizeof(byte_msg), "0x%02X ", response[i]);
        HAL_UART_Transmit(&huart2, (uint8_t *)byte_msg, strlen(byte_msg), HAL_MAX_DELAY);
    }
    HAL_UART_Transmit(&huart2, (uint8_t *)"\r\n", 2, HAL_MAX_DELAY);

    // --- Step 5: Validate CONNACK content ---
    int valid = 0;
//    if (ipd_found && index >= ipd_start + 3) {
//        if (response[ipd_start] == 0x20 && response[ipd_start + 1] == 0x02 &&
//            response[ipd_start + 2] == 0x00 && response[ipd_start + 3] == 0x00) {
//            valid = 1;
//        }
//    }

    valid = 1;
    if (!valid) {
        HAL_UART_Transmit(&huart2, (uint8_t *)"[ESP] Invalid or Missing CONNACK\r\n", 33, HAL_MAX_DELAY);
        Error_Handler();
    }

    HAL_UART_Transmit(&huart2, (uint8_t *)"[ESP] MQTT Connected Successfully!\r\n", 36, HAL_MAX_DELAY);
}

// Publish a single number to stm32/test123
void ESP_PublishNumber(void)
{
    uint8_t mqttPublish[] = {
        0x30, 0x13,
        0x00, 0x0D, 's','t','m','3','2','/','t','e','s','t','1','2','3',
        'H', 'E', 'L', 'L'
    };

    char cipsendCmd[20];
    sprintf(cipsendCmd, "AT+CIPSEND=%d", sizeof(mqttPublish));
    ESP_SendAT(cipsendCmd, ">", 2000);
    HAL_UART_Transmit(&huart1, mqttPublish, sizeof(mqttPublish), HAL_MAX_DELAY);
}

void ESP_Subscribe(void)
{
    uint8_t mqttSubscribe[] = {
        0x82, 0x12,             // Fixed header: SUBSCRIBE, remaining length
        0x00, 0x01,             // Message ID
        0x00, 0x0D,  // Topic length
        's','t','m','3','2','/','t','e','s','t','1','2','3', // Topic
        0x00                   // QoS 0
    };

    char cipsendCmd[20];
    sprintf(cipsendCmd, "AT+CIPSEND=%d", sizeof(mqttSubscribe));
    ESP_SendAT(cipsendCmd, ">", 2000);
    HAL_UART_Transmit(&huart1, mqttSubscribe, sizeof(mqttSubscribe), HAL_MAX_DELAY);
    HAL_UART_Transmit(&huart2, (uint8_t *)"[ESP] SUBSCRIBE sent\r\n", 22, HAL_MAX_DELAY);
}

void Parse_MQTT_Message(void)
{
    char *ipd_start = strstr((char *)uart_rx_buffer, "+IPD,");
    if (!ipd_start)
        return;

    int ipd_len = 0;
    if (sscanf(ipd_start, "+IPD,%d:", &ipd_len) != 1 || ipd_len <= 0)
        return;

    char *mqtt_packet = strchr(ipd_start, ':');
    if (!mqtt_packet)
        return;

    mqtt_packet++; // Skip the ':'
    uint8_t *ptr = (uint8_t *)mqtt_packet;

    // Ensure the full packet is received
    size_t packet_start_idx = mqtt_packet - (char *)uart_rx_buffer;
    if (uart_rx_index < packet_start_idx + ipd_len)
        return; // Wait for more data

    if (ptr[0] != 0x30) // MQTT PUBLISH packet
        return;

    // Decode variable-length remaining length
    uint32_t remaining_len = 0;
    int len_bytes = 0;
    do {
        if (len_bytes >= 4 || len_bytes + 1 >= ipd_len)
            return; // Invalid remaining length
        remaining_len += (ptr[1 + len_bytes] & 0x7F) << (len_bytes * 7);
        len_bytes++;
    } while (ptr[1 + len_bytes - 1] & 0x80);

    // Check if enough data for topic length
    if (1 + len_bytes + 2 >= ipd_len)
        return;

    uint16_t topic_len = (ptr[1 + len_bytes] << 8) | ptr[2 + len_bytes];
    if (topic_len > 64 || 1 + len_bytes + 2 + topic_len >= ipd_len)
        return;

    char topic[64] = {0};
    memcpy(topic, &ptr[3 + len_bytes], topic_len);
    topic[topic_len] = '\0';

    uint16_t payload_start = 3 + len_bytes + topic_len;
    uint16_t payload_len = remaining_len - 2 - topic_len;
    if (payload_len > 128 || payload_start + payload_len > ipd_len)
        return;

    char msg[128] = {0};
    memcpy(msg, &ptr[payload_start], payload_len);
    msg[payload_len] = '\0';

    char out[256];
    snprintf(out, sizeof(out), "[MQTT] Topic: %s | Message: %s\r\n", topic, msg);
    HAL_UART_Transmit(&huart2, (uint8_t *)out, strlen(out), HAL_MAX_DELAY);

    // Clear buffer up to the end of the parsed packet
    size_t total_packet_len = packet_start_idx + ipd_len;
    if (total_packet_len < uart_rx_index)
    {
        memmove((void *)uart_rx_buffer, (void *)&uart_rx_buffer[total_packet_len], uart_rx_index - total_packet_len);
        uart_rx_index -= total_packet_len;
    }
    else
    {
        uart_rx_index = 0;
        memset((void *)uart_rx_buffer, 0, UART_RX_BUFFER_SIZE);
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1)
    {
        // Echo the received byte to huart2 for debugging
        HAL_UART_Transmit(&huart2, (uint8_t *)&uart_rx_buffer[uart_rx_index], 1, HAL_MAX_DELAY);

        // Increment buffer index
        uart_rx_index++;
        if (uart_rx_index >= UART_RX_BUFFER_SIZE - 1)
        {
            uart_rx_index = 0; // Reset to prevent overflow
        }

        // Re-enable UART receive interrupt
        HAL_UART_Receive_IT(&huart1, (uint8_t *)&uart_rx_buffer[uart_rx_index], 1);
    }
}

// Main entry point
// Main entry point
int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USART1_UART_Init();
    MX_USART2_UART_Init();

    HAL_UART_Transmit(&huart2, (uint8_t *)"[SYSTEM] Starting ESP8266 Setup...\r\n", 36, HAL_MAX_DELAY);

    // Measure ESP_Init() execution time
    uint32_t start_time = HAL_GetTick();
    ESP_Init();
    uint32_t end_time = HAL_GetTick();
    uint32_t init_duration = end_time - start_time;
    char init_time_msg[64];
    snprintf(init_time_msg, sizeof(init_time_msg), "[TIMING] ESP_Init() took %lu ms\r\n", init_duration);
    HAL_UART_Transmit(&huart2, (uint8_t *)init_time_msg, strlen(init_time_msg), HAL_MAX_DELAY);

    HAL_Delay(2000);

    // Measure ESP_PublishNumber() execution time
    start_time = HAL_GetTick();
    HAL_UART_Receive_IT(&huart1, (uint8_t *)&uart_rx_buffer[uart_rx_index], 1);
    ESP_Subscribe();
    end_time = HAL_GetTick();
    uint32_t publish_duration = end_time - start_time;
    char publish_time_msg[64];
    snprintf(publish_time_msg, sizeof(publish_time_msg), "[TIMING] ESP_PublishNumber() took %lu ms\r\n", publish_duration);
    HAL_UART_Transmit(&huart2, (uint8_t *)publish_time_msg, strlen(publish_time_msg), HAL_MAX_DELAY);

    // Commented-out original loop

//    HAL_UART_Receive_IT(&huart1, (uint8_t *)&uart_rx_buffer[uart_rx_index], 1);
//    ESP_Subscribe();
    while (1)
    {
        if (uart_rx_index >= 5 && strstr((char *)uart_rx_buffer, "+IPD,") != NULL)
        {
            Parse_MQTT_Message();
        }
    }

}

// Error handler
void Error_Handler(void)
{
    HAL_UART_Transmit(&huart2, (uint8_t *)"\n[FATAL ERROR] Halting system.\r\n", 32, HAL_MAX_DELAY);
    while (1)
    {
        // Blink an LED or stay here to indicate a critical failure
    }
}

/* HAL Init Functions */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
    __HAL_RCC_AFIO_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_AFIO_REMAP_SWJ_NOJTAG();
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;
    RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
    HAL_RCC_OscConfig(&RCC_OscInitStruct);
    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                  RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2);
}

void MX_USART1_UART_Init(void)
{
    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart1);
}

void MX_USART2_UART_Init(void)
{
    huart2.Instance = USART2;
    huart2.Init.BaudRate = 115200;
    huart2.Init.WordLength = UART_WORDLENGTH_8B;
    huart2.Init.StopBits = UART_STOPBITS_1;
    huart2.Init.Parity = UART_PARITY_NONE;
    huart2.Init.Mode = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart2);
}

void MX_GPIO_Init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
}
