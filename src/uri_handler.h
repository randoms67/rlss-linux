#pragma once

#include <string>

// Login redirects back into the app (custom URL schemes such as eos.<id>://)
namespace urihandler {
void onOpenUrl(const std::string& url);  // register the redirect scheme of a login URL
void startListener();                    // receive links for this app instance
int deliver(const std::string& uri);     // `apk-launcher --deliver-uri <link>`
}
