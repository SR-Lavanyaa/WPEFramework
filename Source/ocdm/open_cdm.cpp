/*
 * Copyright 2016-2017 TATA ELXSI
 * Copyright 2016-2017 Metrological
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Module.h"
#include "open_cdm.h"
#include "DataExchange.h"
#include "IOCDM.h"
#include "open_cdm_impl.h"

MODULE_NAME_DECLARATION(BUILD_REFERENCE)

using namespace WPEFramework;

Core::CriticalSection _systemLock;
static const char EmptyString[] = { '\0' };

// TODO: figure out how to force linking of libocdm.so
void ForceLinkingOfOpenCDM()
{
}

static KeyStatus CDMState(const OCDM::ISession::KeyStatus state) {

    switch(state) {
        case OCDM::ISession::StatusPending:    return KeyStatus::StatusPending;
        case OCDM::ISession::Usable:           return KeyStatus::Usable;
        case OCDM::ISession::InternalError:    return KeyStatus::InternalError;
        case OCDM::ISession::Released:         return KeyStatus::Released;
        case OCDM::ISession::Expired:          return KeyStatus::Expired;
        default:           assert(false); 
    }
 
    return KeyStatus::InternalError;
}

struct ExtendedOpenCDMSession : public OpenCDMSession {
private:
    ExtendedOpenCDMSession() = delete;
    ExtendedOpenCDMSession(const ExtendedOpenCDMSession&) = delete;
    ExtendedOpenCDMSession& operator= (ExtendedOpenCDMSession&) = delete;

    enum sessionState {
        // Initialized.
        SESSION_INIT    = 0x00,

        // ExtendedOpenCDMSession created, waiting for message callback.
        SESSION_MESSAGE = 0x01,
        SESSION_READY   = 0x02,
        SESSION_ERROR   = 0x04,
        SESSION_LOADED  = 0x08,
        SESSION_UPDATE  = 0x10
    };

private:
    class Sink : public OCDM::ISession::ICallback {
    private:
        Sink() = delete;
        Sink(const Sink&) = delete;
        Sink& operator= (const Sink&) = delete;
    public:
        Sink(ExtendedOpenCDMSession* parent) 
            : _parent(*parent) {
            ASSERT(parent != nullptr);
        }
        virtual ~Sink() {
        }

    public:
        // Event fired when a key message is successfully created.
        virtual void OnKeyMessage(
            const uint8_t* keyMessage, //__in_bcount(f_cbKeyMessage)
            const uint16_t keyLength, //__in
            const std::string URL) {
            _parent.OnKeyMessage(std::string(reinterpret_cast<const char*>(keyMessage), keyLength), URL);
        }
        // Event fired when MediaKeySession has found a usable key.
        virtual void OnKeyReady() {
            _parent.OnKeyReady();
        }
        // Event fired when MediaKeySession encounters an error.
        virtual void OnKeyError(
            const int16_t error,
            const OCDM::OCDM_RESULT sysError,
            const std::string errorMessage) {
            _parent.OnKeyError(error, sysError, errorMessage);
        }
        // Event fired on key status update
        virtual void OnKeyStatusUpdate(const OCDM::ISession::KeyStatus keyMessage) {
            _parent.OnKeyStatusUpdate(keyMessage);
        }

        BEGIN_INTERFACE_MAP(Sink)
            INTERFACE_ENTRY(OCDM::ISession::ICallback)
        END_INTERFACE_MAP
 
    private:
        ExtendedOpenCDMSession& _parent;
    };

public:
    ExtendedOpenCDMSession(
        OCDM::IAccessorOCDM* system,
        const string keySystem, 
        const std::string& initDataType, 
        const uint8_t* pbInitData, 
        const uint16_t cbInitData, 
        const uint8_t* pbCustomData, 
        const uint16_t cbCustomData, 
        const LicenseType licenseType,
        OpenCDMSessionCallbacks * callbacks)
        : OpenCDMSession()
        , _sink(this)
        , _state(SESSION_INIT)
        , _message()
        , _URL()
        , _error()
        , _errorCode(0)
        , _sysError(0)
        , _key(OCDM::ISession::StatusPending)
        , _callback(callbacks) {

        std::string bufferId;
        OCDM::ISession* realSession = nullptr;

        system->CreateSession(keySystem, licenseType, initDataType, pbInitData, cbInitData, pbCustomData, cbCustomData, &_sink, _sessionId, realSession);

        if (realSession == nullptr) {
            TRACE_L1("Creating a Session failed. %d", __LINE__);
        }
        else {
            OpenCDMSession::Session(realSession);
        }
    }
    virtual ~ExtendedOpenCDMSession() {
        if (OpenCDMSession::IsValid() == true) {
            Revoke(&_sink);
            OpenCDMSession::Session(nullptr);
        }
    }

public:
    virtual bool IsExtended() const override {
        return (true);
    }
    inline KeyStatus Status (const uint8_t /* keyId */[], uint8_t /* length */) const {
        return (::CDMState(_key));
    }
    inline uint32_t Error() const {
        return (_errorCode);
    }
    inline uint32_t Error(const uint8_t keyId[], uint8_t length) const {
        return (_sysError);
    }
    void GetKeyMessage(std::string& challenge, uint8_t* licenseURL, uint16_t& urlLength) {

        ASSERT (IsValid() == true);

        _state.WaitState(SESSION_MESSAGE|SESSION_READY, WPEFramework::Core::infinite);

        if ((_state & SESSION_MESSAGE) == SESSION_MESSAGE) {
            challenge = _message;
            if (urlLength > static_cast<int>(_URL.length())) {
                urlLength = static_cast<uint16_t>(_URL.length());
            }
            memcpy(licenseURL, _URL.c_str(), urlLength);
            TRACE_L1("Returning a KeyMessage, Length: [%d,%d]", urlLength, static_cast<uint32_t>(challenge.length()));
        }
        else if ((_state & SESSION_READY) == SESSION_READY) {
            challenge.clear();
            *licenseURL = '\0';
            urlLength = 0;
            TRACE_L1("Returning a KeyMessage failed. %d", __LINE__);
        }
    }
    int Load (std::string& response) { 
        int ret = 1;

        _state = static_cast<sessionState>(_state & (~(SESSION_UPDATE|SESSION_MESSAGE)));

        response.clear();

        if (OpenCDMSession::Load() == 0) {

            _state.WaitState(SESSION_UPDATE, WPEFramework::Core::infinite);

            if (_key == OCDM::ISession::Usable) {
                ret = 0;
            }
            else if (_state  == SESSION_MESSAGE) {
                ret = 0;
                response = "message:" + _message;
            }
        }

        return ret;
    }
    KeyStatus Update(const uint8_t* pbResponse, const uint16_t cbResponse, std::string& response) {

        _state = static_cast<sessionState>(_state & (~(SESSION_UPDATE|SESSION_MESSAGE)));

        OpenCDMSession::Update(pbResponse, cbResponse);

        _state.WaitState(SESSION_UPDATE | SESSION_MESSAGE, WPEFramework::Core::infinite);
        if ((_state & SESSION_MESSAGE) == SESSION_MESSAGE) {
            response = "message:" + _message;
        }

        return CDMState(_key);
    }
    int Remove(std::string& response) {
        int ret = 1;

        _state =  static_cast<sessionState>(_state & (~(SESSION_UPDATE|SESSION_MESSAGE)));

        if (OpenCDMSession::Remove() == 0) {

            _state.WaitState(SESSION_UPDATE, WPEFramework::Core::infinite);

            if (_key ==  OCDM::ISession::StatusPending) {
                ret = 0;
            }
            else if (_state  == SESSION_MESSAGE) {
                ret = 0;
                response = "message:" + _message;
            }
        }

        return (ret);
    }

private:
    // void (*process_challenge) (void * userData, const char url[], const uint8_t challenge[], const uint16_t challengeLength);
    // void (*key_update)        (void * userData, const uint8_t keyId[], const uint8_t length);
    // void (*message)           (void * userData, const char message[]);

    // Event fired when a key message is successfully created.
    void OnKeyMessage(const std::string& keyMessage, const std::string& URL) {
        _message = keyMessage;
        _URL = URL;
        TRACE_L1("Received URL: [%s]", _URL.c_str());

        if (_callback == nullptr) {
            _state = static_cast<sessionState>(_state | SESSION_MESSAGE | SESSION_UPDATE);
        }
        else {
            _callback->process_challenge(this, _URL.c_str(), reinterpret_cast<const uint8_t*>(_message.c_str()), static_cast<uint16_t>(_message.length()));
        }
    }
    // Event fired when MediaKeySession has found a usable key.
    void OnKeyReady() {
        _key = OCDM::ISession::Usable;
        if (_callback == nullptr) {
            _state = static_cast<sessionState>(_state | SESSION_READY | SESSION_UPDATE);
        }
        else {
            _callback->key_update(this, nullptr, 0);
        }
    }
    // Event fired when MediaKeySession encounters an error.
    void OnKeyError(const int16_t error, const OCDM::OCDM_RESULT sysError, const std::string& errorMessage) {
        _key = OCDM::ISession::InternalError;
        _error = errorMessage;
        _errorCode = error;
        _sysError = sysError;

        if (_callback == nullptr) {
            _state = static_cast<sessionState>(_state | SESSION_ERROR | SESSION_UPDATE);
        }
        else {
            _callback->key_update(this, nullptr, 0);
            _callback->message(this, errorMessage.c_str());
        }
    }
    // Event fired on key status update
    void OnKeyStatusUpdate(const OCDM::ISession::KeyStatus status) {
        _key = status;

        if (_callback == nullptr) {
            _state = static_cast<sessionState>(_state | SESSION_READY | SESSION_UPDATE);
        }
        else {
            _callback->key_update(this, nullptr, 0);
        }
    }

private:
    WPEFramework::Core::Sink<Sink> _sink;
    WPEFramework::Core::StateTrigger<sessionState> _state;
    std::string _message;
    std::string _URL;
    std::string _error;
    uint32_t _errorCode;
    OCDM::OCDM_RESULT _sysError;
    OCDM::ISession::KeyStatus _key;
    OpenCDMSessionCallbacks* _callback;
};

/* static */ OpenCDMAccessor* OpenCDMAccessor::_singleton = nullptr;

namespace media {

OpenCdm::OpenCdm() : _implementation (OpenCDMAccessor::Instance()), _session(nullptr), _keySystem() {
}

OpenCdm::OpenCdm(const OpenCdm& copy) : _implementation (OpenCDMAccessor::Instance()), _session(copy._session), _keySystem(copy._keySystem) {
    
    if (_session != nullptr) {
        TRACE_L1 ("Created a copy of OpenCdm instance: %p", this);
        _session->AddRef();
    }
}

OpenCdm::OpenCdm(const std::string& sessionId) : _implementation (OpenCDMAccessor::Instance()), _session(nullptr), _keySystem() {

    if (_implementation != nullptr) {

        OCDM::ISession* entry = _implementation->Session(sessionId);

        if (entry != nullptr) {
            _session = new OpenCDMSession(entry);
            TRACE_L1 ("Created an OpenCdm instance: %p from session %s, [%p]", this, sessionId.c_str(), entry);
            entry->Release();
        }
        else {
            TRACE_L1 ("Failed to create an OpenCdm instance, for session %s", sessionId.c_str());
        }
    }
    else {
        TRACE_L1 ("Failed to create an OpenCdm instance: %p for session %s", this, sessionId.c_str());
    }
}

OpenCdm::OpenCdm (const uint8_t keyId[], const uint8_t length)  : _implementation (OpenCDMAccessor::Instance()), _session(nullptr), _keySystem() {

     if (_implementation != nullptr) {

         OCDM::ISession* entry = _implementation->Session(keyId, length);

         if (entry != nullptr) {
             _session = new OpenCDMSession(entry);
             // TRACE_L1 ("Created an OpenCdm instance: %p from keyId [%p]", this, entry);
             entry->Release();
         }
         else {
             TRACE_L1 ("Failed to create an OpenCdm instance, for keyId [%d]", __LINE__);
         }
     }
     else {
         TRACE_L1 ("Failed to create an OpenCdm instance: %p for keyId failed", this);
     }
}

OpenCdm::~OpenCdm() {
    if (_session != nullptr) {
        _session->Release();
        TRACE_L1 ("Destructed an OpenCdm instance: %p", this);
    }
    if (_implementation != nullptr) {
        _implementation->Release();
    }
}

/* static */ OpenCdm& OpenCdm::Instance() {
    return Core::SingletonType<OpenCdm>::Instance();
}

// ---------------------------------------------------------------------------------------------
// INSTANTIATION OPERATIONS:
// ---------------------------------------------------------------------------------------------
// Before instantiating the ROOT DRM OBJECT, Check if it is capable of decrypting the requested
// asset.
bool OpenCdm::GetSession (const uint8_t keyId[], const uint8_t length, const uint32_t waitTime) {

    if ( (_session == nullptr) && (_implementation != nullptr) &&
            (_implementation->WaitForKey (length, keyId, waitTime, OCDM::ISession::Usable) == true) ) {
        _session = new OpenCDMSession(_implementation->Session(keyId, length));
    }

    return (_session != nullptr);
}

bool OpenCdm::IsTypeSupported(const std::string& keySystem, const std::string& mimeType) const {
    TRACE_L1("Checking for key system %s", keySystem.c_str());
    return ( (_implementation != nullptr) && 
             (_implementation->IsTypeSupported(keySystem, mimeType) == 0) ); 
}

// The next call is the startng point of creating a decryption context. It select the DRM system 
// to be used within this OpenCDM object.
void OpenCdm::SelectKeySystem(const std::string& keySystem) {
    if (_implementation != nullptr) {
        _keySystem = keySystem;
        TRACE_L1("Creation of key system %s succeeded.", _keySystem.c_str());
    }
    else {
        TRACE_L1("Creation of key system %s failed. No valid remote side", keySystem.c_str());
    }
}

// ---------------------------------------------------------------------------------------------
// ROOT DRM OBJECT OPERATIONS:
// ---------------------------------------------------------------------------------------------
// If required, ServerCertificates can be added to this OpenCdm object (DRM Context).
int OpenCdm::SetServerCertificate(const uint8_t* data, const uint32_t dataLength) {

    int result = 1;

    if (_keySystem.empty() == false) {

        ASSERT (_implementation != nullptr);

        TRACE_L1("Set server certificate data %d", dataLength);
        result = _implementation->SetServerCertificate(_keySystem, data, dataLength);
    }
    else {
        TRACE_L1("Setting server certificate failed, there is no key system. %d", __LINE__);
    }

    return result;
}
 
// Now for every particular stream a session needs to be created. Create a session for all
// encrypted streams that require decryption. (This allows for MultiKey decryption)
std::string OpenCdm::CreateSession(const std::string& dataType, const uint8_t* addData, const uint16_t addDataLength, const uint8_t* cdmData, const uint16_t cdmDataLength, const LicenseType license) {

    std::string result;

    if (_keySystem.empty() == false) {

        ASSERT (_session == nullptr);

        ExtendedOpenCDMSession* newSession = new ExtendedOpenCDMSession (_implementation, _keySystem, dataType, addData, addDataLength, cdmData, cdmDataLength, static_cast<::LicenseType>(license), nullptr);

        result = newSession->SessionId();

        _session = newSession;

        TRACE_L1("Created an OpenCdm instance: %p for keySystem %s, %p", this, _keySystem.c_str(), newSession);
    }
    else {
        TRACE_L1("Creating session failed, there is no key system. %d", __LINE__);
    }

    return (result);
}

// ---------------------------------------------------------------------------------------------
// ROOT DRM -> SESSION OBJECT OPERATIONS:
// ---------------------------------------------------------------------------------------------
// The following operations work on a Session. There is no direct access to the session that
// requires the operation, so before executing the session operation, first select it with
// the SelectSession above.
void OpenCdm::GetKeyMessage(std::string& response, uint8_t* data, uint16_t& dataLength) {

    ASSERT ( (_session != nullptr) && (_session->IsExtended() == true) );

    // Oke a session has been selected. Operation should take place on this session.
    static_cast<ExtendedOpenCDMSession*>(_session)->GetKeyMessage(response, data, dataLength);
}

KeyStatus OpenCdm::Update(const uint8_t* data, const uint16_t dataLength, std::string& response) {

    ASSERT ( (_session != nullptr) && (_session->IsExtended() == true) );

    // Oke a session has been selected. Operation should take place on this session.
    return (static_cast<ExtendedOpenCDMSession*>(_session)->Update(data, dataLength, response));
}

int OpenCdm::Load(std::string& response) {

    ASSERT ( (_session != nullptr) && (_session->IsExtended() == true) );

    // Oke a session has been selected. Operation should take place on this session.
    return (static_cast<ExtendedOpenCDMSession*>(_session)->Load(response));
}

int OpenCdm::Remove(std::string& response) {

    ASSERT ( (_session != nullptr) && (_session->IsExtended() == true) );

    // Oke a session has been selected. Operation should take place on this session.
    return (static_cast<ExtendedOpenCDMSession*>(_session)->Remove(response));
}

KeyStatus OpenCdm::Status() const {
    KeyStatus result = StatusPending;

    if (_session != nullptr) {
        result = CDMState(_session->Status());
    }

    return (result);
}

int OpenCdm::Close() {

    ASSERT ( (_session != nullptr) && (_session->IsExtended() == true) );

    if (_session != nullptr) {
        _session->Close();
        _session->Release();
        _session = nullptr;
    }

    return (0);
}

uint32_t OpenCdm::Decrypt(uint8_t* encrypted, const uint32_t encryptedLength, const uint8_t* IV, const uint16_t IVLength, uint32_t initWithLast15) {
    ASSERT (_session != nullptr);

    return (_session != nullptr ? _session->Decrypt(encrypted, encryptedLength, IV, IVLength, nullptr, 0, initWithLast15) : 1);
}

uint32_t OpenCdm::Decrypt(uint8_t* encrypted, const uint32_t encryptedLength, const uint8_t* IV, const uint16_t IVLength, const uint8_t keyIdLength, const uint8_t keyId[], uint32_t initWithLast15, const uint32_t waitTime) {

    if (_implementation->WaitForKey (keyIdLength, keyId, waitTime, OCDM::ISession::Usable) == true) {
        if (_session == nullptr) {
            _session = new OpenCDMSession(_implementation->Session(keyId, keyIdLength));
        }
        return (_session->Decrypt(encrypted, encryptedLength, IV, IVLength, keyId, keyIdLength, initWithLast15));
    }

    return (1);
}

} // namespace media

/**
 * \brief Creates DRM system.
 *
 * \param keySystem Name of required key system (See \ref opencdm_is_type_supported)
 * \return \ref OpenCDMAccessor instance, NULL on error.
 */
struct OpenCDMAccessor* opencdm_create_system() {
    return (OpenCDMAccessor::Instance());
}

/**
 * Destructs an \ref OpenCDMAccessor instance.
 * \param system \ref OpenCDMAccessor instance to desctruct.
 * \return Zero on success, non-zero on error.
 */
OpenCDMError opencdm_destruct_system(struct OpenCDMAccessor* system) {
    if (system != nullptr) {
        system->Release();
    }
    return (OpenCDMError::ERROR_NONE);
}

/**
 * \brief Checks if a DRM system is supported.
 *
 * \param keySystem Name of required key system (e.g. "com.microsoft.playready").
 * \param mimeType MIME type.
 * \return Zero if supported, Non-zero otherwise.
 * \remark mimeType is currently ignored.
 */
OpenCDMError opencdm_is_type_supported(struct OpenCDMAccessor* system, const char keySystem[], const char mimeType[]) {
    OpenCDMError result (OpenCDMError::ERROR_KEYSYSTEM_NOT_SUPPORTED);

    if ( (system != nullptr) && (system->IsTypeSupported(std::string(keySystem), std::string(mimeType)) == 0) ) {
        result = OpenCDMError::ERROR_NONE;
    }
    return (result);
}

/**
 * \brief Maps key ID to \ref OpenCDMSession instance.
 *
 * In some situations we only have the key ID, but need the specific \ref OpenCDMSession instance that
 * belongs to this key ID. This method facilitates this requirement.
 * \param keyId Array containing key ID.
 * \param length Length of keyId array.
 * \param maxWaitTime Maximum allowed time to block (in miliseconds).
 * \return \ref OpenCDMSession belonging to key ID, or NULL when not found or timed out. This instance
 *         also needs to be destructed using \ref opencdm_session_destruct.
 * REPLACING: void* acquire_session(const uint8_t* keyId, const uint8_t keyLength, const uint32_t waitTime);
 */
struct OpenCDMSession* opencdm_get_session(struct OpenCDMAccessor* system, const uint8_t keyId[], const uint8_t length, const uint32_t waitTime) {
    struct OpenCDMSession* result = nullptr;

    if ( (system != nullptr) && (system->WaitForKey (length, keyId, waitTime, OCDM::ISession::Usable) == true) ) {
        OCDM::ISession* session (system->Session(keyId, length));

        if (session != nullptr) {
            result = new OpenCDMSession(session);
        }
    }

    return (result);
}

/**
 * \brief Sets server certificate.
 *
 * Some DRMs (e.g. WideVine) use a system-wide server certificate. This method will set that certificate. Other DRMs will ignore this call.
 * \param serverCertificate Buffer containing certificate data.
 * \param serverCertificateLength Buffer length of certificate data.
 * \return Zero on success, non-zero on error.
 */
OpenCDMError opencdm_system_set_server_certificate(struct OpenCDMAccessor* system, const char keySystem[], const uint8_t serverCertificate[], uint16_t serverCertificateLength) {
    OpenCDMError result (ERROR_INVALID_ACCESSOR);

    if (system != nullptr) {
        result  = static_cast<OpenCDMError>(system->SetServerCertificate(keySystem, serverCertificate, serverCertificateLength));
    }
    return (result);
}

/**
 * \brief Create DRM session (for actual decrypting of data).
 *
 * Creates an instance of \ref OpenCDMSession using initialization data.
 * \param keySystem DRM system to create the session for.
 * \param licenseType DRM specifc signed integer selecting License Type (e.g. "Limited Duration" for PlayReady).
 * \param initDataType Type of data passed in \ref initData.
 * \param initData Initialization data.
 * \param initDataLength Length (in bytes) of initialization data.
 * \param CDMData CDM data.
 * \param CDMDataLength Length (in bytes) of \ref CDMData.
 * \param session Output parameter that will contain pointer to instance of \ref OpenCDMSession.
 * \return Zero on success, non-zero on error.
 */
OpenCDMError opencdm_create_session(struct OpenCDMAccessor* system, const char keySystem[], const LicenseType licenseType,
                                    const char initDataType[], const uint8_t initData[], const uint16_t initDataLength,
                                    const uint8_t CDMData[], const uint16_t CDMDataLength, OpenCDMSessionCallbacks * callbacks,
                                    struct OpenCDMSession** session) {
    OpenCDMError result (ERROR_INVALID_ACCESSOR);

    if (system != nullptr) {
        *session = new ExtendedOpenCDMSession(static_cast<OCDM::IAccessorOCDM*>(system), std::string(keySystem), std::string(initDataType), initData, initDataLength,CDMData,CDMDataLength, licenseType, callbacks);

        result = (*session != nullptr ? OpenCDMError::ERROR_NONE : OpenCDMError::ERROR_INVALID_SESSION);
    }

    return (result);
}

/**
 * Destructs an \ref OpenCDMSession instance.
 * \param system \ref OpenCDMSession instance to desctruct.
 * \return Zero on success, non-zero on error.
 * REPLACING: void release_session(void* session);
 */
OpenCDMError opencdm_destruct_session(struct OpenCDMSession* session) {
    OpenCDMError result (OpenCDMError::ERROR_INVALID_SESSION);

    if (session != nullptr) {
        result = OpenCDMError::ERROR_NONE;
        session->Release();
    }

    return (result);
}

/**
 * Loads the data stored for a specified OpenCDM session into the CDM context.
 * \param session \ref OpenCDMSession instance.
 * \return Zero on success, non-zero on error.
 */
OpenCDMError opencdm_session_load(struct OpenCDMSession * session) {
    OpenCDMError result (ERROR_INVALID_SESSION);

    if (session != nullptr) {
        result  = static_cast<OpenCDMError>(session->Load());
    }

    return (result);
}

/**
 * Gets session ID for a session.
 * \param session \ref OpenCDMSession instance.
 * \return ExtendedOpenCDMSession ID, valid as long as \ref session is valid.
 */
const char * opencdm_session_id(const struct OpenCDMSession * session) {
    const char* result = EmptyString;
    if (session != nullptr) {
        result = session->SessionId().c_str();
    }
    return (result);
}

/**
 * Gets buffer ID for a session.
 * \param session \ref OpenCDMSession instance.
 * \return Buffer ID, valid as long as \ref session is valid.
 */
const char * opencdm_session_buffer_id(const struct OpenCDMSession * session) {
    const char* result = EmptyString;
    if (session != nullptr) {
        result = session->BufferId().c_str();
    }
    return (result);
}

/**
 * Returns status of a particular key assigned to a session.
 * \param session \ref OpenCDMSession instance.
 * \param keyId Key ID.
 * \param length Length of key ID buffer (in bytes).
 * \return key status.
 */
KeyStatus opencdm_session_status(const struct OpenCDMSession * session, const uint8_t keyId[], uint8_t length) {
    KeyStatus result (KeyStatus::InternalError);

    if ( (session != nullptr) && (session->IsExtended() == true)) {
        result  = static_cast<const ExtendedOpenCDMSession*>(session)->Status(keyId, length);
    }

    return (result);
}

/**
 * Returns error for key (if any).
 * \param session \ref OpenCDMSession instance.
 * \param keyId Key ID.
 * \param length Length of key ID buffer (in bytes).
 * \return Key error (zero if no error, non-zero if error).
 */
uint32_t opencdm_session_error(const struct OpenCDMSession * session, const uint8_t keyId[], uint8_t length) {
    uint32_t result (~0);

    if ( (session != nullptr) && (session->IsExtended() == true)) {
        result  = static_cast<const ExtendedOpenCDMSession*>(session)->Error(keyId, length);
    }

    return (result);
}

/**
 * Returns system error. This reference general system, instead of specific key.
 * \param session \ref OpenCDMSession instance.
 * \return System error code, zero if no error.
 */
OpenCDMError opencdm_session_system_error(const struct OpenCDMSession * session) {
    OpenCDMError result (ERROR_INVALID_SESSION);

    if ( (session != nullptr) && (session->IsExtended() == true)) {
        result  = static_cast<OpenCDMError>(static_cast<const ExtendedOpenCDMSession*>(session)->Error());
    }

    return (result);
}

/**
 * Process a key message response.
 * \param session \ref OpenCDMSession instance.
 * \param keyMessage Key message to process.
 * \param keyLength Length of key message buffer (in bytes).
 * \return Zero on success, non-zero on error.
 */
OpenCDMError opencdm_session_update(struct OpenCDMSession * session, const uint8_t keyMessage[], uint16_t keyLength) {
    OpenCDMError result (ERROR_INVALID_SESSION);

    if (session != nullptr) {
        session->Update(keyMessage, keyLength);
        result = OpenCDMError::ERROR_NONE;
    }

    return (result);
}

/**
 * Removes all keys/licenses related to a session.
 * \param session \ref OpenCDMSession instance.
 * \return Zero on success, non-zero on error.
 */
OpenCDMError opencdm_session_remove(struct OpenCDMSession * session) {
    OpenCDMError result (ERROR_INVALID_SESSION);

    if (session != nullptr) {
        result  = static_cast<OpenCDMError>(session->Remove());
    }

    return (result);
}


/**
 * Closes a session.
 * \param session \ref OpenCDMSession instance.
 * \return zero on success, non-zero on error.
 */
OpenCDMError opencdm_session_close(struct OpenCDMSession* session) {
    OpenCDMError result (ERROR_INVALID_SESSION);

    if (session != nullptr) {
        session->Close();
        result = OpenCDMError::ERROR_NONE;
    }

    return (result);
}

/**
 * \brief Performs decryption.
 *
 * This method accepts encrypted data and will typically decrypt it out-of-process (for security reasons). The actual data copying is performed
 * using a memory-mapped file (for performance reasons). If the DRM system allows access to decrypted data (i.e. decrypting is not
 * performed in a TEE), the decryption is performed in-place.
 * \param session \ref OpenCDMSession instance.
 * \param encrypted Buffer containing encrypted data. If applicable, decrypted data will be stored here after this call returns.
 * \param encryptedLength Length of encrypted data buffer (in bytes).
 * \param IV Initial vector (IV) used during decryption.
 * \param IVLength Length of IV buffer (in bytes).
 * \return Zero on success, non-zero on error.
 * REPLACING: uint32_t decrypt(void* session, uint8_t*, const uint32_t, const uint8_t*, const uint16_t);
 */ 
OpenCDMError opencdm_session_decrypt(struct OpenCDMSession* session, uint8_t encrypted[], const uint32_t encryptedLength, const uint8_t * IV, const uint16_t IVLength, uint32_t initWithLast15 /* = 0 */) {
    OpenCDMError result (ERROR_INVALID_SESSION);

    if (session != nullptr) {
        result  = static_cast<OpenCDMError>(session->Decrypt(encrypted, encryptedLength, IV, IVLength, nullptr, 0, initWithLast15));
    }

    return (result);
}


