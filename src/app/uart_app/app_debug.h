/*******************************************************************************
*
*******************************************************************************/

#if !defined(__APP_DEBUG_H__)
#define      __APP_DEBUG_H__

#include <stdbool.h>




/***  Module Macros  **********************************************************/


#define PLACEHOLDER_FOR_APP_SPECIFIC_ON_MSG           \
        case 'n': app_n_onmsg(msg); break;            

//        case 'i': app_i_onmsg(msg); break;
//        case 'm': app_m_onmsg(msg); break;



/***  Module Types  ***********************************************************/


/***  Module Variables  *******************************************************/


/***  Module Function Prototypes  *********************************************/

extern void  app_uart_process(void);
extern void  app_onmsg(app_console_msg_t* msg);

// True while handling a command that arrived on UART2 -- the PKOB4 "USB Serial Device"
// console mirror. For verbs whose answer depends on the port: printf() reaches BOTH
// ports, so a port-specific warning has to be gated on this or it appears on the port
// it does not apply to. Meaningful only from inside a console handler; the input-source
// lock is released at the command boundary.
extern bool  app_debug_input_is_uart2(void);
// extern void app_i_onmsg(app_console_msg_t* pmsg);
// extern void app_n_onmsg(app_console_msg_t* pmsg);
// extern void app_m_onmsg(app_console_msg_t* pmsg);




#endif //!defined(__APP_DEBUG_H__)
