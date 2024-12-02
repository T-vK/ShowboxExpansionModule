#ifndef UNPROTECTED_APPLEMIDISERVER_H
#define UNPROTECTED_APPLEMIDISERVER_H

#include <AppleMidiServer.h>

namespace midi {

class UnprotectedAppleMidiServer : public AppleMidiServer {
public:
    using AppleMidiServer::AppleMidiServer;
    using AppleMidiServer::writeData;
};

} // namespace


#endif // UNPROTECTED_APPLEMIDISERVER_H