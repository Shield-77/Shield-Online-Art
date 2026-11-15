#pragma once

/* Public identifier supplied by the owner; this is not an access token. */
#define SHIELD_DEVIANTART_CLIENT_ID "81410"
#define SHIELD_DEVIANTART_SCOPE "browse"
#define SHIELD_DEVIANTART_REDIRECT "https://shield-77.github.io/Shield-Online-Art/oauth/"
#define SHIELD_DEVIANTART_AUTH_URL "https://www.deviantart.com/oauth2/authorize"
#define SHIELD_DEVIANTART_TOKEN_URL "https://www.deviantart.com/oauth2/token"
#define SHIELD_DEVIANTART_BROWSE_URL "https://www.deviantart.com/api/v1/oauth2/browse/tags"

/* This HTTPS GitHub Pages route must serve oauth/index.html and be added to
 * the app's exact redirect whitelist before phone authorization can work. */
