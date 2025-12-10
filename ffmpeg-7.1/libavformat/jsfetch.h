#ifndef JSFETCH_H
#define JSFETCH_H

void jsfetch_abort_request(void);
int jsfetch_get_return_code(void);
void jsfetch_set_read_timeout(int ms);
void jsfetch_set_fetch_timeout(int ms);
int jsfetch_already_aborted(void);

#endif
