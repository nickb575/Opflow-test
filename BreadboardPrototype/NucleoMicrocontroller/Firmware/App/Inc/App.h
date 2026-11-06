#ifndef ECE480_APP_H
#define ECE480_APP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Call once after the generated GPIO and USART2 initialization. */
void App_Init(void);

/* Service scheduled work; call repeatedly without inserting delays. */
void App_Task(void);

#ifdef __cplusplus
}
#endif

#endif /* ECE480_APP_H */
