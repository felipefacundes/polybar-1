#pragma once

#include <stdio.h>
#include <functional>
#include <string>

#include <alsa/asoundlib.h>

#include "common.hpp"
#include "config.hpp"
// #include "utils/threading.hpp"

LEMONBUDDY_NS

DEFINE_ERROR(alsa_exception);
DEFINE_CHILD_ERROR(alsa_ctl_interface_error, alsa_exception);
DEFINE_CHILD_ERROR(alsa_mixer_error, alsa_exception);

// class definition : alsa_ctl_interface {{{

template<typename T>
void throw_exception(string&& message, int error_code) {
  const char* snd_error = snd_strerror(error_code);
  if (snd_error != nullptr)
    message += ": "+ string{snd_error};
  throw T(message.c_str());
}

class alsa_ctl_interface {
 public:
  explicit alsa_ctl_interface(int numid) {
    int err = 0;

    snd_ctl_elem_info_alloca(&m_info);
    snd_ctl_elem_value_alloca(&m_value);
    snd_ctl_elem_id_alloca(&m_id);

    snd_ctl_elem_id_set_numid(m_id, numid);
    snd_ctl_elem_info_set_id(m_info, m_id);

    if ((err = snd_ctl_open(&m_ctl, ALSA_SOUNDCARD, SND_CTL_NONBLOCK | SND_CTL_READONLY)) < 0)
      throw_exception<alsa_ctl_interface_error>("Could not open control '"+ string{ALSA_SOUNDCARD} +"'", err);

    if ((err = snd_ctl_elem_info(m_ctl, m_info)) < 0)
      throw_exception<alsa_ctl_interface_error>("Could not get control datal", err);

    snd_ctl_elem_info_get_id(m_info, m_id);

    if ((err = snd_hctl_open(&m_hctl, ALSA_SOUNDCARD, 0)) < 0)
      throw_exception<alsa_ctl_interface_error>("Failed to open hctl", err);
    if ((err = snd_hctl_load(m_hctl)) < 0)
      throw_exception<alsa_ctl_interface_error>("Failed to load hctl", err);
    if ((m_elem = snd_hctl_find_elem(m_hctl, m_id)) == nullptr)
      throw alsa_ctl_interface_error(
          "Could not find control with id " + to_string(snd_ctl_elem_id_get_numid(m_id)));

    if ((err = snd_ctl_subscribe_events(m_ctl, 1)) < 0)
      throw alsa_ctl_interface_error(
          "Could not subscribe to events: " + to_string(snd_ctl_elem_id_get_numid(m_id)));

    // log_trace("Successfully initialized control interface with ID: "+ Intstring(numid));
  }

  ~alsa_ctl_interface() {
    // std::lock_guard<threading_util::spin_lock> lck(m_lock);
    snd_ctl_close(m_ctl);
    snd_hctl_close(m_hctl);
  }

  bool wait(int timeout = -1) {
    assert(m_ctl);

    // std::lock_guard<threading_util::spin_lock> lck(m_lock);

    int err = 0;

    if ((err = snd_ctl_wait(m_ctl, timeout)) < 0)
      throw_exception<alsa_ctl_interface_error>("Failed to wait for events", err);

    snd_ctl_event_t* event;
    snd_ctl_event_alloca(&event);

    if ((err = snd_ctl_read(m_ctl, event)) < 0)
      return false;
    if (snd_ctl_event_get_type(event) != SND_CTL_EVENT_ELEM)
      return false;

    auto mask = snd_ctl_event_elem_get_mask(event);

    return mask & SND_CTL_EVENT_MASK_VALUE;
  }

  bool test_device_plugged() {
    // std::lock_guard<threading_util::spin_lock> lck(m_lock);
    // if (!wait(0))
    //   return false;

    assert(m_elem);
    assert(m_value);

    int err = 0;
    if ((err = snd_hctl_elem_read(m_elem, m_value)) < 0)
      throw_exception<alsa_ctl_interface_error>("Could not read control value", err);
    return snd_ctl_elem_value_get_boolean(m_value, 0);
  }

  void process_events() {}

 private:
  // threading_util::spin_lock m_lock;

  snd_hctl_t* m_hctl = nullptr;
  snd_hctl_elem_t* m_elem = nullptr;

  snd_ctl_t* m_ctl = nullptr;
  snd_ctl_elem_info_t* m_info = nullptr;
  snd_ctl_elem_value_t* m_value = nullptr;
  snd_ctl_elem_id_t* m_id = nullptr;
};

// }}}
// class definition : alsa_mixer {{{

class alsa_mixer {
 public:
  explicit alsa_mixer(string mixer_control_name) {
    snd_mixer_selem_id_t* mixer_id;

    snd_mixer_selem_id_alloca(&mixer_id);

    int err = 0;

    if ((err = snd_mixer_open(&m_hardwaremixer, 1)) < 0)
      throw_exception<alsa_mixer_error>("Failed to open hardware mixer", err);
    if ((err = snd_mixer_attach(m_hardwaremixer, ALSA_SOUNDCARD)) < 0)
      throw_exception<alsa_mixer_error>("Failed to attach hardware mixer control", err);
    if ((err = snd_mixer_selem_register(m_hardwaremixer, nullptr, nullptr)) < 0)
      throw_exception<alsa_mixer_error>("Failed to register simple mixer element", err);
    if ((err = snd_mixer_load(m_hardwaremixer)) < 0)
      throw_exception<alsa_mixer_error>("Failed to load mixer", err);

    snd_mixer_selem_id_set_index(mixer_id, 0);
    snd_mixer_selem_id_set_name(mixer_id, mixer_control_name.c_str());

    if ((m_mixerelement = snd_mixer_find_selem(m_hardwaremixer, mixer_id)) == nullptr)
      throw alsa_mixer_error("Cannot find simple element");

    // log_trace("Successfully initialized mixer: "+ mixer_control_name);
  }

  ~alsa_mixer() {
    // std::lock_guard<threading_util::spin_lock> lck(m_lock);
    snd_mixer_elem_remove(m_mixerelement);
    snd_mixer_detach(m_hardwaremixer, ALSA_SOUNDCARD);
    snd_mixer_close(m_hardwaremixer);
  }

  bool wait(int timeout = -1) {
    assert(m_hardwaremixer);

    // std::lock_guard<threading_util::spin_lock> lck(m_lock);

    int err = 0;

    if ((err = snd_mixer_wait(m_hardwaremixer, timeout)) < 0)
      throw_exception<alsa_mixer_error>("Failed to wait for events", err);

    return process_events() > 0;
  }

  int process_events() {
    int num_events = snd_mixer_handle_events(m_hardwaremixer);

    if (num_events < 0)
      throw_exception<alsa_mixer_error>("Failed to process pending events", num_events);

    return num_events;
  }

  int get_volume() {
    // std::lock_guard<threading_util::spin_lock> lck(m_lock);
    long chan_n = 0, vol_total = 0, vol, vol_min, vol_max;

    snd_mixer_selem_get_playback_volume_range(m_mixerelement, &vol_min, &vol_max);

    for (int i = 0; i < SND_MIXER_SCHN_LAST; i++) {
      if (snd_mixer_selem_has_playback_channel(m_mixerelement, (snd_mixer_selem_channel_id_t)i)) {
        snd_mixer_selem_get_playback_volume(m_mixerelement, (snd_mixer_selem_channel_id_t)i, &vol);
        vol_total += vol;
        chan_n++;
      }
    }

    return 100.0f * (vol_total / chan_n) / vol_max + 0.5f;
  }

  void set_volume(float percentage) {
    if (is_muted())
      return;

    // std::lock_guard<threading_util::spin_lock> lck(m_lock);

    long vol_min, vol_max;

    snd_mixer_selem_get_playback_volume_range(m_mixerelement, &vol_min, &vol_max);
    snd_mixer_selem_set_playback_volume_all(m_mixerelement, vol_max * percentage / 100);
  }

  void set_mute(bool mode) {
    // std::lock_guard<threading_util::spin_lock> lck(m_lock);
    snd_mixer_selem_set_playback_switch_all(m_mixerelement, mode);
  }

  void toggle_mute() {
    // std::lock_guard<threading_util::spin_lock> lck(m_lock);
    int state;
    snd_mixer_selem_get_playback_switch(m_mixerelement, SND_MIXER_SCHN_FRONT_LEFT, &state);
    snd_mixer_selem_set_playback_switch_all(m_mixerelement, !state);
  }

  bool is_muted() {
    // std::lock_guard<threading_util::spin_lock> lck(m_lock);
    int state = 0;
    for (int i = 0; i < SND_MIXER_SCHN_LAST; i++) {
      if (snd_mixer_selem_has_playback_channel(m_mixerelement, (snd_mixer_selem_channel_id_t)i)) {
        snd_mixer_selem_get_playback_switch(m_mixerelement, SND_MIXER_SCHN_FRONT_LEFT, &state);
        if (state == 0)
          return true;
      }
    }

    return false;
  }

 protected:
  void error_handler(string message) {}

 private:
  // threading_util::spin_lock m_lock;

  snd_mixer_t* m_hardwaremixer = nullptr;
  snd_mixer_elem_t* m_mixerelement = nullptr;
};

// }}}

LEMONBUDDY_NS_END
