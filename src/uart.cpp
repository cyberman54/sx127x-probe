/*
 * LMIC Probe - STM32F1x software to monitor LMIC LoRa timings
 * 
 * Copyright (c) 2019 Manuel Bleichenbacher
 * Licensed under MIT License
 * https://opensource.org/licenses/MIT
 * 
 * Asynchronous UART/serial output
 */
#include <stm32f1xx_hal.h>
#include <string.h>
#include "uart.h"


static UART_HandleTypeDef huart1;
static DMA_HandleTypeDef hdma_usart1_tx;

// Buffer for data to be transmitted via UART
//  *  0 <= head < buf_len
//  *  0 <= tail < buf_len
//  *  tail + 1 == head => empty (mod buf_len)
//  *  head == tail => full
#define TX_BUF_LEN 1024
static uint8_t txBuf[TX_BUF_LEN];
static volatile int txBufHead = 1;
static volatile int txBufTail = 0;

// Queue of UART transmission data chunks:
//  *  0 <= head < queue_len
//  *  0 <= tail < queue_len
//  *  head == tail => empty
//  *  head + 1 == tail => full (mod queue_len)
#define TX_QUEUE_LEN 16
static volatile int txChunkBreak[TX_QUEUE_LEN];
static volatile int txQueueHead = 0;
static volatile int txQueueTail = 0;

#define HEX_BUF_LEN 25
static char hexBuf[HEX_BUF_LEN];

static const char* HEX_DIGITS = "0123456789ABCDEF";


static void startUartTransmit();


void uartPrint(const char* str)
{
    uartWrite((const uint8_t*)str, strlen(str));
}

void uartPrintHex(const uint8_t* data, size_t len, _Bool crlf)
{
    while (len > 0) {
        char* p = hexBuf;
        char* end = hexBuf + HEX_BUF_LEN;

        while (len > 0 && p + 4 <= end) {
            uint8_t byte = *data++;
            *p++ = HEX_DIGITS[byte >> 4];
            *p++ = HEX_DIGITS[byte & 0xf];
            *p++ = ' ';
            len--;
        }

        if (len == 0 && crlf)
        {
            p[-1] = '\r';
            *p++ = '\n';
        }

        uartWrite((const uint8_t*)hexBuf, p - hexBuf);
    }
}

void uartWrite(const uint8_t* data, size_t len)
{
    int queueHead = txQueueHead + 1;
    if (queueHead >= TX_QUEUE_LEN)
        queueHead = 0;
    if (queueHead == txQueueTail)
        return; // chunk queue is full
    
    int bufHead = txBufHead;
    int bufTail = txBufTail;
    size_t maxChunkSize = bufHead > bufTail
        ? TX_BUF_LEN - bufHead
        : bufTail - bufHead - 1;
    if (maxChunkSize <= 0)
        return; // tx data buffer is full
    
    // Check for maximum size:
    // - If the free space wraps around, two chunks are used.
    // - If the data to transmit is bigger than the free space,
    //   the remainder is discarded.
    size_t size = len;
    if (size > maxChunkSize)
        size = maxChunkSize;
    
    // Copy data to transmit buffer
    memcpy(txBuf + bufHead, data, size);
    txBufHead += size;
    if (txBufHead >= TX_BUF_LEN)
        txBufHead = 0;

    // append chunk
    txChunkBreak[queueHead] = txBufHead;
    txQueueHead = queueHead;
    
    // start transmission
    startUartTransmit();

    // take care of the remainder (using tail recursion)
    if (len > size)
        uartWrite(data + size, len - size);
}

void startUartTransmit()
{
    __disable_irq();

    if (huart1.gState != HAL_UART_STATE_READY
        || txQueueTail == txQueueHead)
    {
        __enable_irq();
        return; // UART busy or queue empty
    }

    int queueTail = txQueueTail;
    int startPos = txChunkBreak[queueTail];
    queueTail++;
    if (queueTail >= TX_QUEUE_LEN)
        queueTail = 0;
    int endPos = txChunkBreak[queueTail];
    if (endPos == 0)
        endPos = TX_BUF_LEN;

    HAL_UART_Transmit_DMA(&huart1, txBuf + startPos, endPos - startPos);

    __enable_irq();
}

extern "C" void HAL_UART_TxCpltCallback(UART_HandleTypeDef* huart)
{
    __disable_irq();

    int queueTail = txQueueTail + 1;
    if (queueTail >= TX_QUEUE_LEN)
        queueTail = 0;
    int bufTail = txBufTail;
    bufTail += huart->TxXferSize;
    if (bufTail >= TX_BUF_LEN)
        bufTail -= TX_BUF_LEN;
    txBufTail = bufTail;
    txQueueTail = queueTail;

    __enable_irq();

    startUartTransmit();
}


void uartInit()
{
    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart1);

    // DMA1_Channel4_IRQn interrupt configuration
    HAL_NVIC_SetPriority(DMA1_Channel4_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(DMA1_Channel4_IRQn);

    txChunkBreak[txQueueHead] = txBufHead;
}

extern "C" void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    if (huart->Instance == USART1)
    {
        // Peripheral clock enable
        __HAL_RCC_USART1_CLK_ENABLE();

        __HAL_RCC_GPIOA_CLK_ENABLE();
        // USART1 GPIO Configuration
        // PA9     ------> USART1_TX
        // PA10     ------> USART1_RX
        GPIO_InitStruct.Pin = GPIO_PIN_9;
        GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

        GPIO_InitStruct.Pin = GPIO_PIN_10;
        GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

        // USART1 DMA Init
        // USART1_TX Init
        hdma_usart1_tx.Instance = DMA1_Channel4;
        hdma_usart1_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
        hdma_usart1_tx.Init.PeriphInc = DMA_PINC_DISABLE;
        hdma_usart1_tx.Init.MemInc = DMA_MINC_ENABLE;
        hdma_usart1_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
        hdma_usart1_tx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
        hdma_usart1_tx.Init.Mode = DMA_NORMAL;
        hdma_usart1_tx.Init.Priority = DMA_PRIORITY_LOW;
        HAL_DMA_Init(&hdma_usart1_tx);

        __HAL_LINKDMA(huart, hdmatx, hdma_usart1_tx);

        // USART interrupt is needed for completion callback
        HAL_NVIC_SetPriority(USART1_IRQn, 0, 0);
        HAL_NVIC_EnableIRQ(USART1_IRQn);
    }
}

extern "C" void HAL_UART_MspDeInit(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1)
    {
        // Peripheral clock disable
        __HAL_RCC_USART1_CLK_DISABLE();

        // USART1 GPIO Configuration
        // PA9     ------> USART1_TX
        // PA10     ------> USART1_RX
        HAL_GPIO_DeInit(GPIOA, GPIO_PIN_9 | GPIO_PIN_10);

        // USART1 DMA DeInit
        HAL_DMA_DeInit(huart->hdmatx);

        HAL_NVIC_DisableIRQ(USART1_IRQn);
    }
}

extern "C" void DMA1_Channel4_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_usart1_tx);
}

extern "C" void USART1_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart1);
}