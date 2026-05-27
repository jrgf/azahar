// Copyright 2022 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <span>
#include <vector>
#include <boost/serialization/binary_object.hpp>

#include "common/common_types.h"
#include "core/hle/service/nfc/nfc_results.h"
#include "core/hle/service/nfc/nfc_types.h"
#include "core/hle/service/service.h"

namespace Kernel {
class KEvent;
class KReadableEvent;
} // namespace Kernel

namespace Service::NFC {
class NfcDevice {
public:
    NfcDevice(Core::System& system_);
    ~NfcDevice();

    bool LoadAmiibo(std::string filename);
    void UnloadAmiibo();
    void CloseAmiibo();

    void Initialize();
    void Finalize();

    HLE::Result StartCommunication();
    HLE::Result StopCommunication();
    HLE::Result StartDetection(TagProtocol allowed_protocol);
    HLE::Result StopDetection();
    HLE::Result Mount();
    HLE::Result MountAmiibo();
    HLE::Result PartiallyMount();
    HLE::Result PartiallyMountAmiibo();
    HLE::Result ResetTagScanState();
    HLE::Result Flush();

    HLE::Result GetTagInfo2(TagInfo2& tag_info) const;
    HLE::Result GetTagInfo(TagInfo& tag_info) const;
    HLE::Result GetCommonInfo(CommonInfo& common_info) const;
    HLE::Result GetModelInfo(ModelInfo& model_info) const;
    HLE::Result GetRegisterInfo(RegisterInfo& register_info) const;
    HLE::Result GetAdminInfo(AdminInfo& admin_info) const;

    HLE::Result DeleteRegisterInfo();
    HLE::Result SetRegisterInfoPrivate(const RegisterInfoPrivate& register_info);
    HLE::Result RestoreAmiibo();
    HLE::Result Format();

    HLE::Result OpenApplicationArea(u32 access_id);
    HLE::Result GetApplicationAreaId(u32& application_area_id) const;
    HLE::Result GetApplicationArea(std::vector<u8>& data) const;
    HLE::Result SetApplicationArea(std::span<const u8> data);
    HLE::Result CreateApplicationArea(u32 access_id, std::span<const u8> data);
    HLE::Result RecreateApplicationArea(u32 access_id, std::span<const u8> data);
    HLE::Result DeleteApplicationArea();
    HLE::Result ApplicationAreaExist(bool& has_application_area);

    constexpr u32 GetApplicationAreaSize() const;
    DeviceState GetCurrentState() const;
    HLE::Result GetCommunicationStatus(CommunicationState& status) const;
    HLE::Result CheckConnectionState() const;

    std::shared_ptr<Kernel::Event> GetActivateEvent() const;
    std::shared_ptr<Kernel::Event> GetDeactivateEvent() const;

    /// Automatically removes the nfc tag after x ammount of time.
    /// If called multiple times the counter will be restarted.
    void RescheduleTagRemoveEvent();

private:
    time_t GetCurrentTime() const;
    void SetAmiiboName(AmiiboSettings& settings, const AmiiboName& amiibo_name);
    AmiiboDate GetAmiiboDate() const;
    u64 RemoveVersionByte(u64 application_id) const;
    void UpdateSettingsCrc();
    void UpdateRegisterInfoCrc();

    void BuildAmiiboWithoutKeys();

    std::shared_ptr<Kernel::Event> tag_in_range_event = nullptr;
    std::shared_ptr<Kernel::Event> tag_out_of_range_event = nullptr;
    Core::TimingEventType* remove_amiibo_event = nullptr;

    bool is_initalized{};
    bool is_data_moddified{};
    bool is_app_area_open{};
    bool is_plain_amiibo{};
    bool is_write_protected{};
    bool is_tag_in_range{};
    TagProtocol allowed_protocols{};
    DeviceState device_state{DeviceState::NotInitialized};
    ConnectionState connection_state = ConnectionState::Success;
    CommunicationState communication_state = CommunicationState::Idle;

    std::string amiibo_filename = "";

    SerializableAmiiboFile tag{};
    SerializableEncryptedAmiiboFile encrypted_tag{};
    Core::System& system;

    template <class Archive>
    void serialize(Archive& ar, const unsigned int);
    friend class boost::serialization::access;
};

} // namespace Service::NFC

SERVICE_CONSTRUCT(Service::NFC::NfcDevice)
