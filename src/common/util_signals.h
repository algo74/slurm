#ifndef _UTIL_SIGNALS_H
#define _UTIL_SIGNALS_H
void posix_signal_pipe_ignore () ;
void posix_signal_ignore ( int signal ) ;
int unblock_all_signals_pthread ( ) ;
int block_all_signals_pthread ( ) ;
int unblock_all_signals ( ) ;
int block_all_signals ( ) ;
#endif
