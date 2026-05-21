// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#include "network/network.h"

#include "network/artic_base/artic_base_client.h"
#include "network/socket_manager.h"

namespace {

const Network::RoomInformation empty_room_information{};
const Network::Room::BanList empty_ban_list{};
const Network::RoomMember::MemberList empty_member_list{};
const std::string empty_string{};
const Network::MacAddress empty_mac_address{};
std::shared_ptr<Network::Room> stub_room;
std::shared_ptr<Network::RoomMember> stub_room_member;

} // namespace

namespace Network {

class Room::RoomImpl {};

class RoomMember::RoomMemberImpl {};

bool Init() {
    stub_room = std::make_shared<Room>();
    stub_room_member = std::make_shared<RoomMember>();
    return true;
}

std::weak_ptr<Room> GetRoom() {
    return stub_room;
}

std::weak_ptr<RoomMember> GetRoomMember() {
    return stub_room_member;
}

void Shutdown() {
    stub_room_member.reset();
    stub_room.reset();
}

std::atomic<u32> SocketManager::count{};

void SocketManager::EnableSockets() {}

void SocketManager::DisableSockets() {}

Room::Room() = default;

Room::~Room() = default;

Room::State Room::GetState() const {
    return State::Closed;
}

const RoomInformation& Room::GetRoomInformation() const {
    return empty_room_information;
}

std::string Room::GetVerifyUID() const {
    return {};
}

std::vector<Room::Member> Room::GetRoomMemberList() const {
    return {};
}

bool Room::HasPassword() const {
    return false;
}

bool Room::Create(const std::string&, const std::string&, const std::string&, u16,
                  const std::string&, const u32, const std::string&, const std::string&, u64,
                  std::unique_ptr<VerifyUser::Backend>, const BanList&) {
    return false;
}

void Room::SetVerifyUID(const std::string&) {}

Room::BanList Room::GetBanList() const {
    return empty_ban_list;
}

void Room::Destroy() {}

RoomMember::RoomMember() = default;

RoomMember::~RoomMember() = default;

RoomMember::State RoomMember::GetState() const {
    return State::Idle;
}

const RoomMember::MemberList& RoomMember::GetMemberInformation() const {
    return empty_member_list;
}

const std::string& RoomMember::GetNickname() const {
    return empty_string;
}

const std::string& RoomMember::GetUsername() const {
    return empty_string;
}

const MacAddress& RoomMember::GetMacAddress() const {
    return empty_mac_address;
}

RoomInformation RoomMember::GetRoomInformation() const {
    return empty_room_information;
}

bool RoomMember::IsConnected() const {
    return false;
}

void RoomMember::Join(const std::string&, const std::string&, const char*, u16, u16,
                      const MacAddress&, const std::string&, const std::string&) {}

void RoomMember::SendWifiPacket(const WifiPacket&) {}

void RoomMember::SendChatMessage(const std::string&) {}

void RoomMember::SendGameInfo(const GameInfo&) {}

void RoomMember::SendModerationRequest(RoomMessageTypes, const std::string&) {}

void RoomMember::RequestBanList() {}

RoomMember::CallbackHandle<RoomMember::State> RoomMember::BindOnStateChanged(
    std::function<void(const State&)>) {
    return {};
}

RoomMember::CallbackHandle<RoomMember::Error> RoomMember::BindOnError(
    std::function<void(const Error&)>) {
    return {};
}

RoomMember::CallbackHandle<WifiPacket> RoomMember::BindOnWifiPacketReceived(
    std::function<void(const WifiPacket&)>) {
    return {};
}

RoomMember::CallbackHandle<RoomInformation> RoomMember::BindOnRoomInformationChanged(
    std::function<void(const RoomInformation&)>) {
    return {};
}

RoomMember::CallbackHandle<ChatEntry> RoomMember::BindOnChatMessageRecieved(
    std::function<void(const ChatEntry&)>) {
    return {};
}

RoomMember::CallbackHandle<StatusMessageEntry> RoomMember::BindOnStatusMessageReceived(
    std::function<void(const StatusMessageEntry&)>) {
    return {};
}

RoomMember::CallbackHandle<Room::BanList> RoomMember::BindOnBanListReceived(
    std::function<void(const Room::BanList&)>) {
    return {};
}

template <typename T>
void RoomMember::Unbind(CallbackHandle<T>) {}

void RoomMember::Leave() {}

template void RoomMember::Unbind(CallbackHandle<WifiPacket>);
template void RoomMember::Unbind(CallbackHandle<RoomMember::State>);
template void RoomMember::Unbind(CallbackHandle<RoomMember::Error>);
template void RoomMember::Unbind(CallbackHandle<RoomInformation>);
template void RoomMember::Unbind(CallbackHandle<ChatEntry>);
template void RoomMember::Unbind(CallbackHandle<StatusMessageEntry>);
template void RoomMember::Unbind(CallbackHandle<Room::BanList>);

namespace ArticBase {

bool Client::Request::AddParameterS8(s8) {
    return false;
}

bool Client::Request::AddParameterS16(s16) {
    return false;
}

bool Client::Request::AddParameterS32(s32) {
    return false;
}

bool Client::Request::AddParameterS64(s64) {
    return false;
}

bool Client::Request::AddParameterBuffer(const void*, size_t) {
    return false;
}

Client::Request::Request(u32, const std::string& method, size_t max_params)
    : method_name(method), max_param_count(max_params) {}

void Client::UDPStream::Start() {}

void Client::UDPStream::Handle() {}

Client::~Client() {
    SocketManager::DisableSockets();
}

bool Client::Connect() {
    currRequestID = 0;
    connected = false;
    return false;
}

std::shared_ptr<Client::UDPStream> Client::NewUDPStream(
    const std::string, size_t, const std::chrono::milliseconds&) {
    return {};
}

void Client::LogOnServer(ArticBaseCommon::LogOnServerType, const std::string&) {}

void Client::SignalCommunicationError(const std::string& msg) {
    if (communication_error_callback) {
        communication_error_callback(msg);
    }
}

void Client::StopImpl(bool) {
    stopped = true;
}

void Client::PingFunction() {}

std::optional<std::pair<void*, size_t>> Client::Response::GetResponseBuffer(u32) const {
    return std::nullopt;
}

std::optional<Client::Response> Client::Send(Request&) {
    return std::nullopt;
}

} // namespace ArticBase
} // namespace Network
