#include "mgba-highscore.h"

#include <mgba/core/blip_buf.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/serialize.h>
#include <mgba/gba/core.h>
#include <mgba/internal/gba/input.h>

#include <mgba-util/crc32.h>
#include <mgba-util/vfs.h>

#define SAMPLES_PER_FRAME_MOVING_AVG_ALPHA (1.0f / 180.0f)
#define SAMPLE_RATE 32768

#define RUMBLE_PWM 35

static mGBACore *core;

struct _mGBACore
{
  HsCore parent_instance;

  struct mCore *core;
  HsSoftwareContext *context;
  guint8 *frame_buffer;

  size_t audio_buffer_size;
  int16_t *audio_buffer;
  float audio_samples_per_frame_avg;

  struct mLogger logger;

  int rumble_up;
  int rumble_down;
  struct mRumble rumble;
};

static void mgba_game_boy_advance_core_init (HsGameBoyAdvanceCoreInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (mGBACore, mgba_core, HS_TYPE_CORE,
                               G_IMPLEMENT_INTERFACE (HS_TYPE_GAME_BOY_ADVANCE_CORE, mgba_game_boy_advance_core_init))

static void
log_cb (struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args)
{
  UNUSED(logger);

  HsLogLevel hs_level;
  switch (level) {
  case mLOG_FATAL:
  case mLOG_ERROR:
    hs_level = HS_LOG_CRITICAL;
    break;
  case mLOG_WARN:
    hs_level = HS_LOG_WARNING;
    break;
  case mLOG_INFO:
    hs_level = HS_LOG_INFO;
    break;
  case mLOG_DEBUG:
  case mLOG_STUB:
  case mLOG_GAME_ERROR:
  default:
    hs_level = HS_LOG_DEBUG;
    break;
  }

  GString *builder = g_string_new (NULL);
  g_string_append (builder, mLogCategoryName (category));
  g_string_append (builder, ": ");
  g_string_vprintf (builder, format, args);

  char *message = g_string_free_and_steal (builder);

  hs_core_log_literal (HS_CORE (core), hs_level, message);

  g_free (message);
}

static void
set_rumble_cb (struct mRumble* rumble, int enable)
{
  if (enable)
    ++core->rumble_up;
  else
    ++core->rumble_down;

  UNUSED(rumble);
}

static gboolean
mgba_core_load_rom (HsCore      *core,
                    const char **rom_paths,
                    int          n_rom_paths,
                    const char  *save_path,
                    GError     **error)
{
  mGBACore *self = MGBA_CORE (core);

  unsigned width, height;

  g_assert (n_rom_paths == 1);

  self->core->desiredVideoDimensions (self->core, &width, &height);

  self->context = hs_core_create_software_context (HS_CORE (self),
                                                   width, height,
                                                   HS_PIXEL_FORMAT_R8G8B8X8);
  self->frame_buffer = g_new0 (guint8, width * height * 4);

  self->core->setVideoBuffer (self->core, (void *) self->frame_buffer, width);

  if (!mCoreLoadFile (self->core, rom_paths[0])) {
    g_set_error (error, HS_CORE_ERROR, HS_CORE_ERROR_COULDNT_LOAD_ROM, "Failed to load ROM");

    return FALSE;
  }

  const char *bios_path = hs_core_query_firmware_path (HS_CORE (self), HS_GAME_BOY_ADVANCE_FIRMWARE_BIOS);
  if (bios_path) {
    struct VFile* bios = VFileOpen (bios_path, O_RDONLY);
    self->core->loadBIOS (self->core, bios, 0);
  }

  self->core->reset (self->core);

  mCoreLoadSaveFile (self->core, save_path, FALSE);

  return TRUE;
}

static void
mgba_core_start (HsCore *core)
{
  mGBACore *self = MGBA_CORE (core);

  self->core->reset (self->core);
}

static gboolean
mgba_core_reset (HsCore *core, gboolean hard, GError **error)
{
  mGBACore *self = MGBA_CORE (core);

  self->core->reset (self->core);

  self->rumble_up = 0;
  self->rumble_down = 0;

  UNUSED(hard);

  return TRUE;
}

static void
mgba_core_stop (HsCore *core)
{
  mGBACore *self = MGBA_CORE (core);

  g_clear_object (&self->context);
  g_clear_pointer (&self->frame_buffer, g_free);
}

const int BUTTON_MAPPING[] = {
  GBA_KEY_UP,
  GBA_KEY_DOWN,
  GBA_KEY_LEFT,
  GBA_KEY_RIGHT,
  GBA_KEY_A,
  GBA_KEY_B,
  GBA_KEY_SELECT,
  GBA_KEY_START,
  GBA_KEY_L,
  GBA_KEY_R,
};

static void
mgba_core_poll_input (HsCore *core, HsInputState *input_state)
{
  mGBACore *self = MGBA_CORE (core);

  uint32_t keys = 0;
  uint32_t buttons = input_state->game_boy_advance.buttons;

  for (int btn = 0; btn < HS_GAME_BOY_ADVANCE_N_BUTTONS; btn++) {
    if (buttons & 1 << btn)
      keys |= 1 << BUTTON_MAPPING[btn];
  }

  self->core->setKeys (self->core, keys);
}

static void
mgba_core_run_frame (HsCore *core)
{
  mGBACore *self = MGBA_CORE (core);

  self->core->runFrame (self->core);

  unsigned width, height;
  self->core->desiredVideoDimensions (self->core, &width, &height);

  guint8 *fb = hs_software_context_acquire_framebuffer (self->context);
  memcpy (fb, self->frame_buffer, width * height * 4);
  hs_software_context_release_framebuffer (self->context);

  blip_t *left_channel  = self->core->getAudioChannel (self->core, 0);
  blip_t *right_channel = self->core->getAudioChannel (self->core, 1);
  int available = blip_samples_avail (left_channel);

  if (available > 0) {
    size_t samples_to_read;

    /* Update 'running average' of number of
     * samples per frame.
     * Note that this is not a true running
     * average, but just a leaky-integrator/
     * exponential moving average, used because
     * it is simple and fast (i.e. requires no
     * window of samples). */
    self->audio_samples_per_frame_avg =
      (SAMPLES_PER_FRAME_MOVING_AVG_ALPHA * (float) available) +
       ((1.0f - SAMPLES_PER_FRAME_MOVING_AVG_ALPHA) * self->audio_samples_per_frame_avg);

    samples_to_read = (size_t) self->audio_samples_per_frame_avg;

    /* Resize audio output buffer, if required */
    if (self->audio_buffer_size < samples_to_read * 2) {
      self->audio_buffer_size = samples_to_read * 2;
      self->audio_buffer = realloc (self->audio_buffer, self->audio_buffer_size * sizeof(int16_t));
    }

    int produced = blip_read_samples (left_channel, self->audio_buffer, samples_to_read, true);
    blip_read_samples (right_channel, self->audio_buffer + 1, samples_to_read, true);

    if (produced > 0)
      hs_core_play_samples (core, self->audio_buffer, produced * 2);
  }

  if (self->rumble_up) {
    double strength = (double) self->rumble_up / (double) (self->rumble_up + self->rumble_down);
    hs_core_rumble (HS_CORE (core), 0, strength, strength, HS_MAX_RUMBLE_DURATION);
  } else {
    hs_core_rumble (HS_CORE (core), 0, 0, 0, HS_MAX_RUMBLE_DURATION);
  }

  self->rumble_up = 0;
  self->rumble_down = 0;
}

static gboolean
mgba_core_reload_save (HsCore      *core,
                       const char  *save_path,
                       GError     **error)
{
  UNUSED(error);

  mGBACore *self = MGBA_CORE (core);

  mCoreLoadSaveFile (self->core, save_path, FALSE);

  return TRUE;
}

static void
mgba_core_load_state (HsCore          *core,
                      const char      *path,
                      HsStateCallback  callback)
{
  mGBACore *self = MGBA_CORE (core);
  struct VFile* vf = VFileOpen (path, O_RDONLY);

  mCoreLoadStateNamed (self->core, vf, SAVESTATE_SAVEDATA | SAVESTATE_RTC);

  vf->close (vf);

  callback (core, NULL);
}

static void
mgba_core_save_state (HsCore          *core,
                      const char      *path,
                      HsStateCallback  callback)
{
  mGBACore *self = MGBA_CORE (core);
  struct VFile* vf = VFileOpen (path, O_CREAT | O_TRUNC | O_RDWR);

  mCoreSaveStateNamed (self->core, vf, SAVESTATE_SAVEDATA | SAVESTATE_RTC);

  vf->close (vf);

  callback (core, NULL);
}

static double
mgba_core_get_frame_rate (HsCore *core)
{
  mGBACore *self = MGBA_CORE (core);

  return self->core->frequency (self->core) / (double) self->core->frameCycles (self->core);
}

static double
mgba_core_get_aspect_ratio (HsCore *core)
{
  mGBACore *self = MGBA_CORE (core);
  unsigned width, height;

  self->core->desiredVideoDimensions (self->core, &width, &height);

  return (double) width / (double) height;
}

static double
mgba_core_get_sample_rate (HsCore *core)
{
  UNUSED(core);

  return SAMPLE_RATE;
}

static void
mgba_core_constructed (GObject *object)
{
  mGBACore *self = MGBA_CORE (object);

  G_OBJECT_CLASS (mgba_core_parent_class)->constructed (object);

  self->core = GBACoreCreate ();

  mCoreInitConfig (self->core, NULL);

  struct mCoreOptions opts = {
    .useBios = true,
  };
  mCoreConfigLoadDefaults (&self->core->config, &opts);

  self->logger.log = log_cb;
  mLogSetDefaultLogger(&self->logger);

  self->rumble.setRumble = set_rumble_cb;

  self->core->init (self->core);

  size_t samples_per_frame;

  samples_per_frame = SAMPLE_RATE / mgba_core_get_frame_rate (HS_CORE (self));

  self->audio_buffer_size = samples_per_frame * 2;
  self->audio_buffer = malloc (self->audio_buffer_size * sizeof(int16_t));
  self->audio_samples_per_frame_avg = (float) samples_per_frame;

  /* Internal audio buffer size should be
   * audioSamplesPerFrame, but number of samples
   * actually generated varies slightly on a
   * frame-by-frame basis. We therefore allow
   * for some wriggle room by setting double
   * what we need (accounting for the hard
   * coded blip buffer limit of 0x4000). */
  self->core->setAudioBufferSize (self->core, MIN (samples_per_frame * 2, 0x4000));

  blip_set_rates (self->core->getAudioChannel (self->core, 0), self->core->frequency (self->core), SAMPLE_RATE);
  blip_set_rates (self->core->getAudioChannel (self->core, 1), self->core->frequency (self->core), SAMPLE_RATE);

  self->core->setPeripheral (self->core, mPERIPH_RUMBLE, &self->rumble);
}

static void
mgba_core_finalize (GObject *object)
{
  mGBACore *self = MGBA_CORE (object);

  mCoreConfigDeinit (&self->core->config);
  self->core->deinit (self->core);
  if (self->audio_buffer)
    free (self->audio_buffer);

  G_OBJECT_CLASS (mgba_core_parent_class)->finalize (object);

  core = NULL;
}

static void
mgba_core_class_init (mGBACoreClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  HsCoreClass *core_class = HS_CORE_CLASS (klass);

  object_class->constructed = mgba_core_constructed;
  object_class->finalize = mgba_core_finalize;

  core_class->load_rom = mgba_core_load_rom;
  core_class->start = mgba_core_start;
  core_class->reset = mgba_core_reset;
  core_class->stop = mgba_core_stop;
  core_class->poll_input = mgba_core_poll_input;
  core_class->run_frame = mgba_core_run_frame;

  core_class->reload_save = mgba_core_reload_save;

  core_class->load_state = mgba_core_load_state;
  core_class->save_state = mgba_core_save_state;

  core_class->get_frame_rate = mgba_core_get_frame_rate;
  core_class->get_aspect_ratio = mgba_core_get_aspect_ratio;

  core_class->get_sample_rate = mgba_core_get_sample_rate;
}

static void
mgba_core_init (mGBACore *self)
{
  g_assert (!core);

  core = self;
}

static void
mgba_game_boy_advance_core_init (HsGameBoyAdvanceCoreInterface *iface)
{
  UNUSED(iface);
}

GType
hs_get_core_type (void)
{
  return MGBA_TYPE_CORE;
}
