/*
 *
 *  Copyright 2013 Netflix, Inc.
 *
 *     Licensed under the Apache License, Version 2.0 (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */
#include "PpInstance.h"
#include <ppapi/c/ppb_instance.h>
#include <ppapi/c/private/ppb_instance_private.h>
#include <ppapi/c/ppb_var.h>
#include <ppapi/c/dev/ppb_crypto_dev.h>
#include <base/DebugUtil.h>
#include <base/Variant.h>
#include <base/Base64.h>
#include <crypto/CadmiumCrypto.h>
#include "MainThreadUtil.h"
#include "Version.h"
#include "BackgroundDispatcher.h"
#include "NativeBridge.h"
#include "BrowserConsoleLog.h"

using namespace std;
using namespace cadmium::base;
using namespace cadmium::crypto;

namespace   // anonymous
{

const char* const kUsedInterfaces[] = {
  PPB_CORE_INTERFACE,
  PPB_INSTANCE_INTERFACE,
  PPB_INSTANCE_PRIVATE_INTERFACE,
  PPB_VAR_INTERFACE,
  PPB_CRYPTO_DEV_INTERFACE,
  PPB_CONSOLE_INTERFACE
};
const size_t kUsedInterfaceCount = sizeof(kUsedInterfaces) / sizeof(kUsedInterfaces[0]);

// Add entropy after this number of calls to invoke()
const uint32_t kAddEntropyInterval = 100;

typedef std::vector<unsigned char> Vuc;

Vuc getRandBytes()
{
    assert(isMainThread());
    const PPB_Crypto_Dev* const crypto = reinterpret_cast<const PPB_Crypto_Dev*>(
            pp::Module::Get()->GetBrowserInterface(PPB_CRYPTO_DEV_INTERFACE));
    assert(NULL != crypto);
    vector<unsigned char> randBytes(CadmiumCrypto::MIN_SEED_LEN, 0);
    (*crypto->GetRandomBytes)(reinterpret_cast<char*>(&randBytes[0]), randBytes.size());
    return randBytes;
}

}   // namespace anonymous

namespace cadmium
{

PpInstance::PpInstance(PP_Instance instance)
:   pp::InstancePrivate(instance)
,   callbackFactory_(this)
,   backgroundDispatcher_(NULL)
,   msgCount_(0)
{
    FUNCTIONSCOPELOG;
    //LogToBrowserConsole(pp_instance(), PP_LOGLEVEL_LOG, "PpInstance ctor");
}

PpInstance::~PpInstance()
{
    FUNCTIONSCOPELOG;
    LogToBrowserConsole(pp_instance(), PP_LOGLEVEL_LOG, "PpInstance dtor");
    delete backgroundDispatcher_;
}

bool PpInstance::Init(uint32_t argc, const char* argn[], const char* argv[])
{
    FUNCTIONSCOPELOG;
    assert(isMainThread());

    //LogToBrowserConsole(pp_instance(), PP_LOGLEVEL_LOG, "PpInstance::Init");

    // announce
    stringstream banner;
    banner << "Netflix WebCrypto Version " << Plugin_VERSION_MAJOR << "." << Plugin_VERSION_MINOR;
    DLOG() << banner << endl;
    LogToBrowserConsole(pp_instance(), PP_LOGLEVEL_LOG, banner.str().c_str());

    // ensure PPAPI has the interfaces we need
    if (!checkUsedInterfaces())
    {
        DLOG() << "Error: missing required PPAPI interface(s)" << endl;
        LogToBrowserConsole(pp_instance(), PP_LOGLEVEL_ERROR, "Error: missing required PPAPI interface(s)");
        return false;
    }

    // handle input options and query line
    handleOptions(argc, argn, argv);

    // Make an instance of CadmiumCrypto and init it
    cadmiumCrypto_.reset(new CadmiumCrypto());
    if (cadmiumCrypto_->init(getRandBytes()) != CAD_ERR_OK)
    {
        DLOG() << "CadmiumCrypto::init failure\n";
        LogToBrowserConsole(pp_instance(), PP_LOGLEVEL_ERROR, "CadmiumCrypto::init failure");
        return false;
    }

    // Hook the CadmiumCrypto instance to a NativeBridge instance
    nativeBridge_.reset(new NativeBridge(this, cadmiumCrypto_.get()));

    // Make the dispatcher for incoming messages and hook it up to the native bridge
    backgroundDispatcher_ = new BackgroundDispatcher(nativeBridge_.get());
    backgroundDispatcher_->start();

    LogToBrowserConsole(pp_instance(), PP_LOGLEVEL_LOG, "PpInstance: Init complete, sending Ready message");

    nativeBridge_->sendReady(0);    // 0 means success; fail is not an option!

    return true;
}

// Returns true if the matching versions of all Pepper interfaces used by this
// plugin are available.
bool PpInstance::checkUsedInterfaces()
{
    FUNCTIONSCOPELOG;
    DLOG() << "Checking ppapi interfaces:" << endl;

    for (size_t i = 0; i < kUsedInterfaceCount; ++i)
    {
        DLOG() << "\t" << kUsedInterfaces[i] << endl;
        if (!pp::Module::Get()->GetBrowserInterface(kUsedInterfaces[i]))
        {
            string message = "ERROR: Could not find ppapi interface ";
            message += kUsedInterfaces[i];
            DLOG() << message << endl;
            return false;
        }
    }
    DLOG() << "All ppapi interfaces available." << endl;
    return true;
}

void PpInstance::handleOptions(uint32_t argc, const char* argn[],
        const char* argv[])
{
#if 0   // no options for now; consider run_unit_test
    FUNCTIONSCOPELOG;
    string environment;
    for (uint32_t i = 0; i < argc; i++)
    {
        if (string(argn[i]) == "environment")
            environment = argv[i];
    }

    if (environment == "test")
    {
        DLOG() << "--- TEST ENVIRONMENT ---\n";
        mNetflixIdCookieName += "Test";
        mSecureNetflixIdCookieName += "Test";
    }
#endif
}

void PpInstance::HandleMessage(const pp::Var& msgVar)
{
    FUNCTIONSCOPELOG;
    assert(isMainThread());
    if (msgCount_++ > kAddEntropyInterval)
    {
        const Vuc randBytes64 = Base64::encode(getRandBytes());
        const string randBytes64Str(randBytes64.begin(), randBytes64.end());
        VariantMap addEntropyMsg;
        addEntropyMsg["method"] = "addEntropy";
        addEntropyMsg["data"] = randBytes64Str;
        Variant msg = addEntropyMsg;
        backgroundDispatcher_->postMessage(msg.toJSON());
        msgCount_ = 0;
    }
    backgroundDispatcher_->postMessage(msgVar.AsString());
}

}   // namespace cadmium
