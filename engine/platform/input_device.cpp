// Native hardware implementation of the ORIGINAL SIO::Device contract.
// Manager, Inp::Server/Data/Handler and their task ordering remain original.
#include <core/defines.h>
#include <sys/siodev.h>
#include <source_port/input.h>
namespace SIO {
int gLastPadPressed = 0;
Device::Device(int index, int port, int slot)
    : m_node(new Lst::Node<Device>(this)), m_state(vIDLE), m_next_state(vIDLE),
      m_index(index), m_plugged_in(false) {
    m_data.m_type = vANALOG_CTRL;
    m_data.m_port = port; m_data.m_slot = slot; m_data.m_valid = false;
    m_data.m_dma_buff = nullptr; m_data.m_prealbuffer = nullptr;
    m_data.m_button_mode = vDIGITAL; m_data.m_num_actuators = 0;
    m_data.m_actuators_disabled = false;
    std::memset(m_data.m_control_data, 0, CTRL_BUFFER_LENGTH);
    std::memset(m_data.m_actuator_align, 0xff, ACTUATOR_BUFFER_LENGTH);
    std::memset(m_data.m_actuator_direct, 0, ACTUATOR_BUFFER_LENGTH);
    std::memset(m_data.m_actuator_old_direct, 0, ACTUATOR_BUFFER_LENGTH);
    std::memset(m_data.m_actuator_max, 0, ACTUATOR_BUFFER_LENGTH);
    query_capabilities();
}
Device::~Device() { Unacquire(); delete m_node; }
void Device::query_capabilities() { m_data.m_caps = mANALOG; }
void Device::Acquire() { m_state = vACQUIRING; acquisition_pending(); }
void Device::Unacquire() { m_state = vIDLE; m_data.m_valid = m_plugged_in = false; }
int Device::get_status() {
    // Keep the original logical slots: frontend objects bind both players even
    // in an offline one-player game. Only slot zero has physical native input.
    return m_index == 0 && SourcePort::InputConnected() ? vREADY : vDISCONNECTED;
}
void Device::acquisition_pending() {
    if (get_status() == vREADY) { m_state = vACQUIRED; read_data(); }
    else m_data.m_valid = m_plugged_in = false;
}
void Device::read_data() {
    SourcePort::CopyInputPacket(m_data.m_control_data);
    m_data.m_valid = m_plugged_in = get_status() == vREADY;
    if (!m_plugged_in) m_state = vACQUIRING;
    if (m_data.m_valid && (m_data.m_control_data[2] != 0xff || m_data.m_control_data[3] != 0xff))
        gLastPadPressed = m_index;
}
void Device::process() {
    switch (m_state) {
        case vACQUIRED: read_data(); break;
        case vACQUIRING: acquisition_pending(); break;
        case vBUSY: wait(); break;
        default: break;
    }
}
void Device::wait() { m_state = m_next_state; }
bool Device::IsPluggedIn() { return m_plugged_in; }
void Device::ActivatePressureSensitiveMode() {
    // Truthful capabilities: original Inp converts digital button pressure.
    Dbg_Assert(!m_data.m_caps.TestMask(mANALOG_BUTTONS));
    m_data.m_button_mode = vDIGITAL;
}
void Device::DeactivatePressureSensitiveMode() { m_data.m_button_mode = vDIGITAL; }
void Device::ActivateActuator(int act_num, int percent) {
    Dbg_Assert(percent >= 0 && percent <= 100);
    Dbg_Assert(act_num >= 0);
    // New 3DS has no vibration motor; never advertise mACTUATORS.
    Dbg_Assert(!m_data.m_caps.TestMask(mACTUATORS));
}
void Device::Pause() { m_data.m_paused_ref.Acquire(); }
void Device::UnPause() { m_data.m_paused_ref.Release(); }
void Device::DisableActuators() { m_data.m_actuators_disabled = true; }
void Device::EnableActuators() { m_data.m_actuators_disabled = false; }
void Device::ResetActuators() { StopAllVibrationIncludingSaved(); }
void Device::StopAllVibrationIncludingSaved() {
    std::memset(m_data.m_actuator_direct, 0, ACTUATOR_BUFFER_LENGTH);
    std::memset(m_data.m_actuator_old_direct, 0, ACTUATOR_BUFFER_LENGTH);
}
}
