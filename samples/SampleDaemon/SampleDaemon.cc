/* SampleDaemon - Allow thin client applications to slave off it */

/******************************************************************************
 * Copyright 2013, Qualcomm Innovation Center, Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 ******************************************************************************/

#include <alljoyn/BusAttachment.h>
#include <alljoyn/PasswordManager.h>
#include <alljoyn/Status.h>
#include <alljoyn/version.h>

#include <qcc/Debug.h>
#include <qcc/Event.h>
#include <qcc/String.h>
#include <qcc/StringUtil.h>
#include <qcc/Thread.h>

#include <iostream>
#include <vector>

#include <csignal>

#define QCC_MODULE "SAMPLE_DAEMON"

using namespace ajn;
using namespace qcc;
using namespace std;

namespace org {
namespace alljoyn {
namespace SampleDaemon {
const char* DefaultDaemonBusName = "org.alljoyn.BusNode.TestingPurposesOnly";

const char* ThinClientAuthMechanism = "ALLJOYN_PIN_KEYX";
const char* ThinClientDefaultBusPwd = "1234";
}
}
}

static volatile sig_atomic_t g_interrupted = false;
static void SigIntHandler(int sig)
{
    g_interrupted = true;
}

static void usage(void) {
    std::cout << "Usage: SampleDaemon [-h] [-n <name-to-advertise>]\n\n" <<
    "Options:\n" <<
    "   -h                        = Print this help message\n" <<
    "   -n <name-to-advertise>" <<
    "    = Name to be advertised by the SampleDaemon, that thin client apps are looking for\n" <<
    std::endl;
}

int main(int argc, char** argv)
{
    std::cout << "AllJoyn Library version: " << ajn::GetVersion() <<
    "\nAllJoyn Library build info: " << ajn::GetBuildInfo() << std::endl;

    // Register SIGNT (Ctrl-C) handler
    signal(SIGINT, SigIntHandler);

    String nameToAdvertise = ::org::alljoyn::SampleDaemon::DefaultDaemonBusName;

    // Parse command line arguments, if any
    for (int i = 1; i < argc; i++) {
        if (0 == strcmp("-h", argv[i])) {
            usage();
            exit(0);
        } else if (0 == strcmp("-n", argv[i])) {
            if (argc == ++i) {
                std::cout << "option " << argv[i - 1] << " requires a name parameter" << std::endl;
                usage();
                exit(1);
            } else {
                nameToAdvertise = argv[i];
            }
        } else {
            std::cout << "Unknown option " << argv[i] << std::endl;
            usage();
            exit(1);
        }
    }

    BusAttachment msgBus("SampleDaemon", true);

    QStatus status = ER_FAIL;
    status = msgBus.Start();

    if (ER_OK == status) {
        // Set the credential that thin clients have to offer to connect
        // to this SampleDaemon in a trusted manner.
        PasswordManager::SetCredentials(
            ::org::alljoyn::SampleDaemon::ThinClientAuthMechanism,
            ::org::alljoyn::SampleDaemon::ThinClientDefaultBusPwd
            );

        // Force connecting to bundled daemon (i.e. null transport) to ensure
        // that credentials are correctly set.
        //
        // NOTE: The above SetCredentials call doesn't take effect
        //       when connecting to a pre-installed daemon.
        status = msgBus.Connect("null:");

        if (ER_OK == status) {
            // 'Quiet'ly advertise the name to be discovered by thin clients.
            // Also, given that thin clients are in the same network as the
            // SampleDaemon, advertise the name ONLY over TCP Transport.
            nameToAdvertise = "quiet@" + nameToAdvertise;
            status = msgBus.AdvertiseName(nameToAdvertise.c_str(), TRANSPORT_TCP);
            if (ER_OK != status) {
                QCC_LogError(status, ("Unable to quietly advertise the name %s", nameToAdvertise.c_str()));
            }
        }
    }

    // Wait for Ctrl-C to exit
    while (!g_interrupted) {
        qcc::Sleep(100);
    }

    QCC_SyncPrintf("%s exiting with status %u (%s)\n", argv[0], status, QCC_StatusText(status));
    return (int) status;
}
