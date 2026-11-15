#pragma once
#include <stdbool.h>
#include <stddef.h>

#define OAUTH_ACCESS_TOKEN_CAPACITY 2048
#define OAUTH_REFRESH_TOKEN_CAPACITY 2048
typedef struct {
    char access[OAUTH_ACCESS_TOKEN_CAPACITY];
    char refresh[OAUTH_REFRESH_TOKEN_CAPACITY];
    unsigned expires_in;
} OAuthToken;

/* Strictly parses a bounded successful token response; token storage is RAM only. */
bool oauth_token_parse(const char *json, size_t length, OAuthToken *token);
void oauth_token_clear(OAuthToken *token);
