#ifndef ECE480_TEST_MAIN_H
#define ECE480_TEST_MAIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint32_t id; } UART_HandleTypeDef;
typedef struct { uint32_t unused; } GPIO_TypeDef;
typedef struct { volatile uint32_t DEMCR; } CoreDebug_Type;
typedef struct { volatile uint32_t CYCCNT; volatile uint32_t CTRL; } DWT_Type;
typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef enum { GPIO_PIN_RESET, GPIO_PIN_SET } GPIO_PinState;

extern CoreDebug_Type* CoreDebug;
extern DWT_Type* DWT;
extern GPIO_TypeDef* GPIOA;
extern uint32_t SystemCoreClock;

#define GPIO_PIN_5 0x0020U
#define CoreDebug_DEMCR_TRCENA_Msk (1UL << 24)
#define DWT_CTRL_CYCCNTENA_Msk 1UL

uint32_t __get_PRIMASK(void);
void __disable_irq(void);
void __set_PRIMASK(uint32_t mask);
void __DSB(void);
void __ISB(void);
uint32_t HAL_GetTick(void);
void HAL_GPIO_WritePin(GPIO_TypeDef*, uint16_t, GPIO_PinState);
void HAL_GPIO_TogglePin(GPIO_TypeDef*, uint16_t);
HAL_StatusTypeDef HAL_UART_Transmit_IT(UART_HandleTypeDef*, const uint8_t*, uint16_t);
HAL_StatusTypeDef HAL_UART_AbortTransmit(UART_HandleTypeDef*);
void HAL_UART_TxCpltCallback(UART_HandleTypeDef*);
void HAL_UART_ErrorCallback(UART_HandleTypeDef*);

#ifdef __cplusplus
}
#endif
#endif
