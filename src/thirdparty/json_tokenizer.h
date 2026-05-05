/* json_tokenizer.h - public domain data structures - Stefan Elmlund 2024
*
*  This is a single-header-file [STB-style](https://github.com/nothings/stb/blob/master/docs/stb_howto.txt)
*  library for tokenize json file in C (also works in C++)
*
*  The easiest way to install the library to your C/C++ project is to copy 'n' paste the json_tokenizer.h
*  to your project and do this in *one* C or C++ file:
*
*    #define JSON_TOKENIZER_IMPLEMENTATION
*    #include "json_tokenizer.h"
*
*  COMPILE-TIME OPTIONS
*
*    These defines only need to be set in the file containing JSON_TOKENIZER_IMPLEMENTATION
*
*    #define JSON_REALLOC(context,ptr,size) better_realloc
*    #define JSON_FREE(context,ptr)         better_free
*
*      By default the stdlib realloc() and free() is used. You can defines your own by
*      defining these symbols. You must either define both, or neither.
*
*      Note that at the moment, 'context' will always be NULL.
*
*    #define JSON_FOPEN(fp,filename,mode) better_fopen
*    #define JSON_FGETC(fp)               better_fgetc
*    #define JSON_FCLOSE(fp)              better_fclose
*
*      By default the stdlib fopen(), fgetc() and free() is used. You can defines you own
*      by defining these symbols. You most either define all three, or neither
*
*  LICENSE
*
*    Placed in the public domain and also MIT licensed.
*    See end of file for detailed license information.
*
*  EXAMPLE
*
*    You can find examples how to parser a sample json file that contains a person data using C and C++ (C++11).
*
*      main.cpp
*/

#ifndef __JSON_TOKENIZER_H__
#define __JSON_TOKENIZER_H__

#ifdef __cplusplus
extern "C" {
#include <cstdio>
#include <cstdint>
#else
#include <stdio.h>
#include <stdint.h>
#endif

typedef struct json__impl json_t;

typedef enum {
	JSON_START_DOCUMENT,
	JSON_END_DOCUMENT,
	JSON_START_ARRAY,
	JSON_END_ARRAY,
	JSON_START_OBJECT,
	JSON_END_OBJECT,
	JSON_NAME,
	JSON_STRING,
	JSON_INT64,
	JSON_UINT64,
	JSON_DOUBLE,
	JSON_BOOLEAN,
	JSON_NULL,
	JSON_ERROR
} json_token_t;

/** @brief Open a json file for reading.
*   @param filename Name of the xml file.
*   @return NULL on failure och a pointer to a json structure on success.
*/
json_t* json_fopen(const char* filename);

/** @brief Close a json file and free memory for the xml structure.
*   @param json Pointer to the json structure.
*/
void json_close(json_t* json);

/** @brief Read the next token from the json input
*   @param json Pointer to a json structure.
*   @return The next token.
*/
json_token_t json_next_token(json_t* json);

/** @brief Get the name of object, can only be read after a JSON_START_OBJECT token.
*   @param json Pointer to a json structure.
*   @return A string to a name if applicable else NULL.
*/
const char* json_get_name(json_t* json);

/** @brief Get a string, can only be read after a JSON_STRING, JSON_INT, JSON_UINT, JSON_DOUBLE, JSON_BOOLEAN or JSON_NULL token.
*   @param json Pointer to a json structure.
*   @return A string to a string if applicable else NULL.
*/
const char* json_get_value(json_t* json);

/** @brief Get a error message, can only be read after a JSON_STRING token.
*   @return A string with the error i applicable else NULL.
*/
const char* json_get_error(json_t* json);

#ifdef JSON_TOKENIZER_IMPLEMENTATION

#if defined(_REALLOC) && !defined(JSON_FREE) || !defined(JSON_REALLOC) && defined(JSON_FREE)
#error "You must define both JSON_REALLOC and JSON_FREE, or neither."
#endif
#if !defined(JSON_REALLOC) && !defined(JSON_FREE)
#include <stdlib.h>
#define JSON_REALLOC(c,p,s) realloc(p,s)
#define JSON_FREE(c,p)      free(p)
#endif

#if defined(JSON_FOPEN) && !defined(JSON_FGETC) || defined(JSON_FOPEN) && !defined(JSON_FCLOSE)
#error "You must define both JSON_FOPEN, JSON_FGETC and JSON_FCLOSE, or neither."
#endif

#if defined(JSON_FGETC) && !defined(JSON_FOPEN) || defined(JSON_FGETC) && !defined(JSON_FCLOSE)
#error "You must define both JSON_FOPEN, JSON_FGETC and JSON_FCLOSE, or neither."
#endif

#if defined(JSON_FCLOSE) && !defined(JSON_FOPEN) || defined(JSON_FCLOSE) && !defined(JSON_FGETC)
#error "You must define both JSON_FOPEN, JSON_FGETC and JSON_FCLOSE, or neither."
#endif

#if !defined(JSON_FOPEN) && !defined(JSON_FGETC) && !defined(JSON_FCLOSE)
#ifdef _MSC_VER
#define JSON_FOPEN(fp,filename,mode) fopen_s(&(fp),filename,mode)
#else
#define JSON_FOPEN(fp,filename,mode) (((fp=fopen(filename,mode))==NULL)?(-1):(-(feof(fp)||ferror(fp))))
#endif
#define JSON_FGETC(fp) fgetc(fp)
#define JSON_FCLOSE(fp) fclose(fp)
#endif

#define STACK_SIZE (4096)
#define MAX_NESTING_LEVEL (20)
#define LABEL(addr) do{case addr:;}while(0);
#define JMP(addr) do{json->lc=addr;goto jp;}while(0)
#define CALL(ret_addr,call_addr) do{{enum json__label ret=ret_addr; json->lc=call_addr;json__push(json,&ret,sizeof(enum json__label));}goto jp;case ret_addr:;}while(0)
#define RET() do{json->lc=*(enum json__label*)json__pop(json, sizeof(enum json__label));goto jp;}while(0);
#define TOK(addr,tok) do{json->lc=addr;return tok;case addr:;}while(0)

enum json__label {
	json__start, json__error, json__error_loop, json__padding, json__object, json__array, json__array_l1, json__array_l2, json__object_l1,
	json__object_l2, json__string, json__element, json__null, json__true, json__false, json__number, json__number_l1, json__number_l2,
	json__c1, json__c2, json__c3, json__c4, json__c5, json__c6, json__c7, json__c8, json__c9, json__c10, json__c11, json__c12, json__c13, json__c14,
	json__c15, json__c16, json__c17, json__c18, json__t1, json__t2, json__t3, json__t4, json__t5, json__t6, json__t7, json__t8, json__t9, json__t10,
	json__t11, json__t12, json__t13, json__t14
};

enum json__number_type {
	JSON__NUMBER_INT64, JSON__NUMBER_UINT64, JSON__NUMBER_DOUBLE
};

struct json__impl {
	FILE* fp;
	enum json__label lc;
	enum json__number_type number_type;
	int ch, ra, rb, rc, row, col, sc, level;
	size_t stack_capacity;
	uint8_t* stack;
};

const char json__error_unexpected_end_of_file[] = "Error: Unexpected end of file.";
const char json__error_while_reading_file[] = "Error: While reading file, code: ";
const char json__error_prefix[] = "Error(";
const char json__unexpected_sign[] = "): Unexpected sign.";

static void json__push(json_t* json, const void* data, size_t size)
{
	if ((json->sc + size) > json->stack_capacity) {
		size_t new_capacity = json->stack_capacity * 2;
		uint8_t* new_stack = (uint8_t*)JSON_REALLOC(NULL, json->stack, new_capacity);
		if (new_stack == NULL) {
			fprintf(stderr, "PANIC failed to allocate memory for json_t stack!");
			exit(-1);
		}
		json->stack = new_stack;
		json->stack_capacity = new_capacity;
		}
	for (size_t i = 0; i < size; i++) {
		json->stack[json->sc++] = ((uint8_t*)data)[i];
	}
}

static const void* json__pop(json_t* json, size_t size)
{
	json->sc -= (int)size;
	return &(json->stack[json->sc]);
}
static const void* json__peek(json_t* json, size_t size, size_t index)
{
	return &(json->stack[json->sc - size - index]);
}

static const char* json__peek_str(json_t* json)
{
	int size = *((int*)&json->stack[json->sc - sizeof(int) - sizeof(uint8_t)]);
	return (const char*)(json->stack + json->sc - sizeof(int) - size - sizeof(uint8_t));
}

static const char* json__pop_str(json_t* json) {
	int size = *((int*)&json->stack[json->sc - sizeof(int) - sizeof(uint8_t)]);
	const char* str = (const char*)(json->stack + json->sc - sizeof(int) - size - sizeof(uint8_t));
	json->sc -= (sizeof(int) + size + sizeof(uint8_t));
	return str;
}
static size_t json__strlen(const char* str) {
	const char* n = str;
	while (*n != '\0') n++;
	return n - str;
}

static char* json__itoa(char* buf, size_t bufsize, int val, int base)
{
	size_t i = bufsize - 2;
	buf[bufsize - 1] = '\0';

	for (; val && i; --i, val /= base) {
		buf[i] = "0123456789abcdef"[val % base];
	}

	return &buf[i + 1];
}

static void json__push_str(json_t* json, const char* str, uint8_t postfix) {

	int  len = (int)(json__strlen(str) + sizeof(uint8_t));
	json__push(json, str, len);
	json__push(json, &len, sizeof(int));
	json__push(json, &postfix, sizeof(uint8_t));
}

static int json__strncmp(const char* a, const char* b, size_t n)
{
	while (n && *a && (*a == *b)) {
		++a; ++b; n--;
	}
	if (n == 0) return 0;
	else return (*(uint8_t*)a - *(uint8_t*)b);
}

static int json__getc(json_t* json)
{
	int ch = fgetc(json->fp);
	if (ch == '\n') {
		json->row++;
		json->col = 1;
	}
	else {
		json->col++;
	}
	json->ch = ch;
	return ch != EOF;
}

int json__hex_to_int(int ch)
{
	if (ch >= '0' && ch <= '9') return (ch - '0');
	else if (ch >= 'A' && ch <= 'F') return (ch - 'A' + 10);
	else if (ch >= 'a' && ch <= 'f') return (ch - 'a' + 10);
	else return 0;
}

void json__push_hex(json_t* json, const int* const hex)
{
	int unicode = json__hex_to_int(hex[0]) << 12;
	unicode += json__hex_to_int(hex[1]) << 8;
	unicode += json__hex_to_int(hex[2]) << 4;
	unicode += json__hex_to_int(hex[3]);

	if (unicode >= 0 && unicode <= 0x7f) { // 7F(16) = 127(10)
		uint8_t ch = (uint8_t)unicode;
		json__push(json, &ch, sizeof(uint8_t));
	}
	else if (unicode <= 0x7ff)  // 7FF(16) = 2047(10)
	{
		unsigned char c1 = 192, c2 = 128;
		for (int k = 0; k < 11; ++k) {
			if (k < 6)  c2 |= (unicode % 64) & (1 << k);
			else c1 |= (unicode >> 6) & (1 << (k - 6));
		}
		json__push(json, &c1, sizeof(uint8_t));
		json__push(json, &c2, sizeof(uint8_t));
	}
	else if (unicode <= 0xffff) { // FFFF(16) = 65535(10)
		unsigned char c1 = 224, c2 = 128, c3 = 128;
		for (int k = 0; k < 16; ++k) {
			if (k < 6)  c3 |= (unicode % 64) & (1 << k);
			else if (k < 12) c2 |= (unicode >> 6) & (1 << (k - 6));
			else c1 |= (unicode >> 12) & (1 << (k - 12));
		}
		json__push(json, &c1, sizeof(uint8_t));
		json__push(json, &c2, sizeof(uint8_t));
		json__push(json, &c3, sizeof(uint8_t));
	}
}

json_t* json_fopen(const char* filename)
{
	FILE* fp = NULL;

	if (JSON_FOPEN(fp, filename, "r") != 0) {
		return NULL;
	}

	json_t* json = (json_t*)JSON_REALLOC(NULL, NULL, sizeof(json_t));
	if(json == NULL) {
		fprintf(stderr, "PANIC: Failed to allocate memory for svg structure.");
		exit(-1);
	}

	json->stack = (uint8_t*)calloc(STACK_SIZE, sizeof(uint8_t));
	if (json->stack == NULL) {
		fprintf(stderr, "PANIC: Failed to allocate memory for svg stack.");
		exit(-1);
	}

	json->lc = json__start;
	json->col = 1;
	json->row = 1;
	json->sc = 0;
	json->fp = fp;
	json->level = 0;
	json->stack_capacity = STACK_SIZE;

	return json;
}

json_token_t json_next_token(json_t* json)
{
	int sc, len;
	uint8_t ch, n, postfix, pf, comma;
	char buf[32];
jp: switch (json->lc) {
	LABEL(json__start);
	if (!json__getc(json)) JMP(json__error);
	if (json->ch == 0xEF) for (int i = 0; i < 3; i++) { // Ignore BOM
		if(!json__getc(json)) JMP(json__error);
	}
	json->col = 1;
	json->level = 1;
	CALL(json__c1, json__padding);
	if (json->ch == '{') CALL(json__c2, json__object);
	else if (json->ch == '[') CALL(json__c3, json__array);
	else JMP(json__error);
	json__getc(json);
	CALL(json__c18, json__padding);
	if (!feof(json->fp)) JMP(json__error);
	for (;;) TOK(json__t1, JSON_END_DOCUMENT);

	LABEL(json__padding);
	while (json->ch == ' ' || json->ch == '\r' || json->ch == '\n' || json->ch == '\t' || json->ch == '\f') {
		json__getc(json);
	}
	RET();

	LABEL(json__element);
	if (json->ch == '\"') {
		sc = json->sc;
		n = '\0';
		postfix = 's';
		CALL(json__c12, json__string);
		json__push(json, &n, sizeof(uint8_t));
		len = json->sc - sc;
		json__push(json, &len, sizeof(int));
		json__push(json, &postfix, sizeof(uint8_t));
		TOK(json__t5, JSON_STRING);
		json__pop_str(json);
		RET();
	}
	else if (json->ch == '-') {
		json->number_type = JSON__NUMBER_INT64;
		json->ra = json->sc;
		uint8_t ch = '-'; json__push(json, &ch, sizeof(uint8_t));
		json__getc(json);
		if (json->ch >= '0' && json->ch <= '9') JMP(json__number);
		JMP(json__error);
	}
	else if (json->ch >= '1' && json->ch <= '9') {
		json->number_type = JSON__NUMBER_UINT64;
		json->ra = json->sc;
		JMP(json__number);
	}
	else if (json->ch == '0') {
		json->number_type = JSON__NUMBER_INT64;
		json->ra = json->sc;
		uint8_t ch = '0'; json__push(json, &ch, sizeof(uint8_t));
		json__getc(json);
		if (json->ch == '.') JMP(json__number_l1);
		JMP(json__number_l2);
	}
	else switch (json->ch) {
	case '{': json->level++; JMP(json__object); break;
	case '[': json->level++; JMP(json__array); break;
	case 't': JMP(json__true); break;
	case 'f': JMP(json__false); break;
	case 'n': JMP(json__null); break;
	default: JMP(json__error);
	}
	RET();

	LABEL(json__object);
	if (json->level > MAX_NESTING_LEVEL) JMP(json__error);
	TOK(json__t2, JSON_START_OBJECT);
	json__getc(json);
	CALL(json__c5, json__padding);
	LABEL(json__object_l1);
	switch (json->ch) {
	case '\"': break;
	case '}': JMP(json__object_l2);
	default: JMP(json__error);
	}
	{
		sc = json->sc;
		n = '\0';
		postfix = 'n';
		CALL(json__c6, json__string);
		json__push(json, &n, sizeof(uint8_t));
		len = json->sc - sc;
		json__push(json, &len, sizeof(int));
		json__push(json, &postfix, sizeof(uint8_t));
		TOK(json__t3, JSON_NAME);
		json__pop_str(json);
	}
	CALL(json__c7, json__padding);
	if (json->ch != ':') JMP(json__error);
	json__getc(json);
	CALL(json__c8, json__padding);
	CALL(json__c9, json__element);
	CALL(json__c10, json__padding);
	if (json->ch == ',') {
		json__getc(json);
		CALL(json__c11, json__padding);
		if (json->ch == '}') JMP(json__error);
		JMP(json__object_l1);
	}
	else if (json->ch != '}') JMP(json__error);
	LABEL(json__object_l2);
	TOK(json__t4, JSON_END_OBJECT);
	json->level--;
	if (json->level > 0) json__getc(json);
	RET();

	LABEL(json__number); {
		ch = json->ch; json__push(json, &ch, sizeof(uint8_t));
		for (;;) {
			json__getc(json);
			if (json->ch >= '0' && json->ch <= '9') {
				ch = json->ch; json__push(json, &ch, sizeof(uint8_t));
			}
			else break;
		}
		if (json->ch == '.') {
			LABEL(json__number_l1);
			json->number_type = JSON__NUMBER_DOUBLE;
			ch = json->ch; json__push(json, &ch, sizeof(uint8_t));
			for (;;) {
				json__getc(json);
				if (json->ch >= '0' && json->ch <= '9') {
					ch = json->ch; json__push(json, &ch, sizeof(uint8_t));
				}
				else break;
			}
		}
		if (json->ch == 'e' || json->ch == 'E') {
			json->number_type = JSON__NUMBER_DOUBLE;
			ch = json->ch; json__push(json, &ch, sizeof(uint8_t));
			json__getc(json);
			if (json->ch == '+' || json->ch == '-' || (json->ch >= '0' && json->ch <= '9')) {
				ch = json->ch; json__push(json, &ch, sizeof(uint8_t));
			}
			else JMP(json__error);
			for (;;) {
				json__getc(json);
				if (json->ch >= '0' && json->ch <= '9') {
					ch = json->ch; json__push(json, &ch, sizeof(uint8_t));
				}
				else break;
			}
		}
		LABEL(json__number_l2);
		{
			n = '\0';
			json__push(json, &n, sizeof(uint8_t));
			len = json->sc - json->ra;
			json__push(json, &len, sizeof(int));
			if (json->number_type == JSON__NUMBER_UINT64) {
				postfix = 'u';
				json__push(json, &postfix, sizeof(uint8_t));
				TOK(json__t6, JSON_UINT64);
			}
			else if (json->number_type == JSON__NUMBER_INT64) {
				postfix = 'i';
				json__push(json, &postfix, sizeof(uint8_t));
				TOK(json__t7, JSON_INT64);
			}
			else if (json->number_type == JSON__NUMBER_DOUBLE) {
				postfix = 'd';
				json__push(json, &postfix, sizeof(uint8_t));
				TOK(json__t8, JSON_DOUBLE);
			}
			json__pop_str(json);
		}
		json->col--;
	} RET();

	LABEL(json__array);
	if (!json__getc(json)) JMP(json__error);
	if (json->level > MAX_NESTING_LEVEL) JMP(json__error);
	TOK(json__t9, JSON_START_ARRAY);
	CALL(json__c13, json__padding);
	if (json->ch == ']') JMP(json__array_l2);
	LABEL(json__array_l1);
	CALL(json__c14, json__element);
	CALL(json__c15, json__padding);
	if (json->ch == ',') {
		json__getc(json);
		CALL(json__c17, json__padding);
		if (json->ch == ']') JMP(json__error);
		JMP(json__array_l1);
	}
	else if (json->ch == ']') {
		LABEL(json__array_l2);
		TOK(json__t11, JSON_END_ARRAY);
		json->level--;
		if (json->level > 0) json__getc(json);
	}
	else JMP(json__error);
	RET();

	LABEL(json__string); {
		enum json__label lc = (*(enum json__label*)json__pop(json, sizeof(enum json__label)));
		for (;;) {
			json__getc(json);
			if (json->ch == '\"') break;
			else if (json->ch < 0x20 || json->ch > 0x10FFFF) JMP(json__error);
			else if (json->ch == '\\') {
				json__getc(json);
				uint8_t ch;
				switch (json->ch) {
				case '"': ch = '\"';  json__push(json, &ch, sizeof(uint8_t)); break;
				case '/': ch = '/';  json__push(json, &ch, sizeof(uint8_t)); break;
				case '\\': ch = '\\'; json__push(json, &ch, sizeof(uint8_t)); break;
				case 'b': ch = '\b'; json__push(json, &ch, sizeof(uint8_t)); break;
				case 'f': ch = '\f'; json__push(json, &ch, sizeof(uint8_t)); break;
				case 'n': ch = '\n'; json__push(json, &ch, sizeof(uint8_t)); break;
				case 'r': ch = '\r'; json__push(json, &ch, sizeof(uint8_t)); break;
				case 't': ch = '\t'; json__push(json, &ch, sizeof(uint8_t)); break;
				case 'u': {
					int hex[4];
					for (int i = 0; i < 4; i++) {
						json__getc(json);
						if (!((json->ch >= '0' && json->ch <= '9') || (json->ch >= 'a' && json->ch <= 'f') || (json->ch >= 'A' && json->ch <= 'F'))) {
							JMP(json__error);
						}
						hex[i] = json->ch;
					}
					json__push_hex(json, hex);
				} break;
				default: JMP(json__error);
				}
			}
			else {
				uint8_t ch = json->ch; json__push(json, &ch, sizeof(uint8_t));
			}
		}
		json__push(json, &lc, sizeof(enum json__label));
		json__getc(json);
	}
	RET();

	LABEL(json__true); {
		len = 5;
		ch = json->ch; json__push(json, &ch, sizeof(uint8_t));
		n = '\0';
		pf = 'b';
		for (int i = 0; i < 3; i++) {
			if (!json__getc(json)) JMP(json__error);
			ch = json->ch; json__push(json, &ch, sizeof(uint8_t));
		}
		json__push(json, &n, sizeof(uint8_t));
		json__push(json, &len, sizeof(int));
		json__push(json, &pf, sizeof(uint8_t));
		if (json__strncmp("true", json__peek_str(json), 4) == 0) {
			TOK(json__t12, JSON_BOOLEAN);
			json__pop_str(json);
			json__getc(json);
		}
		else JMP(json__error);
	} RET();

	LABEL(json__false); {
		len = 6;
		n = '\0';
		pf = 'b';
		ch = json->ch; json__push(json, &ch, sizeof(uint8_t));
		for (int i = 0; i < 4; i++) {
			if (!json__getc(json)) JMP(json__error);
			ch = json->ch; json__push(json, &ch, sizeof(uint8_t));
		}
		json__push(json, &n, sizeof(uint8_t));
		json__push(json, &len, sizeof(int));
		json__push(json, &pf, sizeof(uint8_t));
		if (json__strncmp("false", json__peek_str(json), 5) == 0) {
			TOK(json__t13, JSON_BOOLEAN);
			json__pop_str(json);
			json__getc(json);
		}
		else JMP(json__error);
	} RET();

	LABEL(json__null);
	len = 5;
	n = '\0';
	pf = 'z';
	ch = json->ch; json__push(json, &ch, sizeof(uint8_t));
	for (int i = 0; i < 3; i++) {
		if (!json__getc(json)) JMP(json__error);
		ch = json->ch; json__push(json, &ch, sizeof(uint8_t));
	}
	json__push(json, &n, sizeof(uint8_t));
	json__push(json, &len, sizeof(int));
	json__push(json, &pf, sizeof(uint8_t));
	if (json__strncmp("null", json__peek_str(json), 4) == 0) {
		TOK(json__t14, JSON_NULL);
		json__pop_str(json);
		json__getc(json);
	}
	else JMP(json__error);
	RET();

	LABEL(json__error);
	{
		sc = json->sc;
		comma = ',';
		pf = 'e';
		if (feof(json->fp)) {
			json__push(json, json__error_unexpected_end_of_file, sizeof(json__error_unexpected_end_of_file));
			int len = (int)sizeof(json__error_unexpected_end_of_file);
			json__push(json, &len, sizeof(int));
			json__push(json, &pf, sizeof(uint8_t));
		}
		else if (ferror(json->fp)) {
			size_t sc = json->sc;
			json__push(json, json__error_while_reading_file, sizeof(json__error_while_reading_file));
			int len = (int)sizeof(json__error_while_reading_file);
			json__push(json, &len, sizeof(int));
			json__push(json, &pf, sizeof(uint8_t));
		}
		else {
			json__push(json, json__error_prefix, sizeof(json__error_prefix) - 1);
			const char* rowstr = json__itoa(buf, sizeof(buf), json->row, 10);
			json__push(json, rowstr, json__strlen(rowstr));
			json__push(json, &comma, sizeof(uint8_t));
			const char* colstr = json__itoa(buf, sizeof(buf), json->col, 10);
			json__push(json, colstr, json__strlen(colstr));
			json__push(json, json__unexpected_sign, sizeof(json__unexpected_sign));
			int len = (int)(json->sc - sc);
			json__push(json, &len, sizeof(int));
			json__push(json, &pf, sizeof(uint8_t));
		}
		for (;;) TOK(json__error_loop, JSON_ERROR);
	}
	default: break;
	}
	return JSON_ERROR;
}

const char* json_get_error(json_t* json) {
	if (json->stack[json->sc - sizeof(uint8_t)] == 'e') {
		int cnt = *(int*)json__peek(json, sizeof(int), sizeof(uint8_t));
		return (const char*)&json->stack[(size_t)json->sc - cnt - sizeof(int) - sizeof(uint8_t)];
	}
	return NULL;
}

const char* json_get_name(json_t* json) {
	if (json->stack[json->sc - sizeof(uint8_t)] == 'n') {
		int cnt = *(int*)json__peek(json, sizeof(int), sizeof(uint8_t));
		return (const char*)&json->stack[(size_t)json->sc - cnt - sizeof(int) - sizeof(uint8_t)];
	}
	return NULL;
}

const char* json_get_value(json_t* json) {
	uint8_t t = json->stack[json->sc - sizeof(uint8_t)];
	if (t == 's' || t == 'u' || t == 'i' || t == 'd' || t == 'b' || t == 'z') {
		int cnt = *(int*)json__peek(json, sizeof(int), sizeof(uint8_t));
		return (const char*)&json->stack[(size_t)json->sc - cnt - sizeof(int) - sizeof(uint8_t)];
	}
	return NULL;
}

void json_close(json_t* json)
{
	JSON_FCLOSE(json->fp);
	JSON_FREE(NULL, json->stack);
	JSON_FREE(NULL, json);
}


#undef STACK_SIZE
#undef MAX_NESTING_LEVEL
#undef LABEL
#undef JMP
#undef CALL
#undef RET
#undef TOK
#undef JSON_PARSER_IMPLEMENTATION

#endif
#ifdef __cplusplus
}
#endif
#endif

/*
------------------------------------------------------------------------------
This software is available under 2 licenses -- choose whichever you prefer.
------------------------------------------------------------------------------
ALTERNATIVE A - MIT License
Copyright (c) 2024 Stefan Elmlund
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
------------------------------------------------------------------------------
ALTERNATIVE B - Public Domain (www.unlicense.org)
This is free and unencumbered software released into the public domain.
Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.
In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain. We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors. We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
------------------------------------------------------------------------------
*/
