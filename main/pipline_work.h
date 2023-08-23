/*
 * file_work.h
 *
 *  Created on: May 29, 2023
 *      Author: devin
 */

#ifndef MAIN_PIPLINE_WORK_H_
#define MAIN_PIPLINE_WORK_H_


#include "board.h"

// header of file2http
void init_file2http();
void deinit_file2http();
void run_file2http(const char *src_url, const char *dst_url);
void enable_file2http(bool enable);

// header of file2player
void init_file2player();
void deinit_file2player();
void run_file2player(const char *src_url, const char *dst_url);
void enable_file2player(bool enable);

// header of http2player
void init_http2player();
void deinit_http2player();
void run_http2player(const char *src_url, const char *dst_url);
void enable_http2player(bool enable);

// header of http2file
void init_http2file();
void deinit_http2file();
void run_http2file(const char *src_url, const char *dst_url);
void enable_http2file(bool enable);

#endif /* MAIN_PIPLINE_WORK_H_ */
