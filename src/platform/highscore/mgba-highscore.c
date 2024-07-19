#include "mgba-highscore.h"

#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/serialize.h>
#include <mgba/gb/core.h>
#include <mgba/gb/interface.h>
#include <mgba/gba/core.h>
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gb/input.h>
#include <mgba/internal/gb/overrides.h>
#include <mgba/internal/gba/input.h>

#include <mgba-util/crc32.h>
#include <mgba-util/image.h>
#include <mgba-util/vfs.h>

#define GB_SAMPLES 512
#define SAMPLES_PER_FRAME_MOVING_AVG_ALPHA (1.0f / 180.0f)

static mGBACore *core;

struct _mGBACore
{
  HsCore parent_instance;

  struct mCore *core;
  struct mAVStream stream;
  HsSoftwareContext *context;

  size_t audio_buffer_size;
  int16_t *audio_buffer;
  float audio_samples_per_frame_avg;

  struct mLogger logger;

  struct mRumbleIntegrator rumble;
};

static void mgba_game_boy_core_init (HsGameBoyCoreInterface *iface);
static void mgba_game_boy_advance_core_init (HsGameBoyAdvanceCoreInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (mGBACore, mgba_core, HS_TYPE_CORE,
                               G_IMPLEMENT_INTERFACE (HS_TYPE_GAME_BOY_CORE, mgba_game_boy_core_init)
                               G_IMPLEMENT_INTERFACE (HS_TYPE_GAME_BOY_ADVANCE_CORE, mgba_game_boy_advance_core_init))

static inline gboolean
is_gba (mGBACore *self)
{
  return hs_core_get_platform (HS_CORE (self)) == HS_PLATFORM_GAME_BOY_ADVANCE;
}

static inline gboolean
is_gb (mGBACore *self)
{
  return hs_core_get_platform (HS_CORE (self)) == HS_PLATFORM_GAME_BOY;
}

static inline void
refresh_screen_area (mGBACore *self)
{
  unsigned width, height;

  self->core->currentVideoSize (self->core, &width, &height);

  hs_software_context_set_area (self->context,
                                &HS_RECTANGLE_INIT (0, 0, width, height));
}

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

  hs_core_log (HS_CORE (core), hs_level, message);

  g_free (message);
}

static void
set_rumble_cb (struct mRumbleIntegrator* rumble, float level)
{
  UNUSED(rumble);

  hs_core_rumble (HS_CORE (core), 0, level, level);
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

  self->core->baseVideoSize (self->core, &width, &height);

  self->context = hs_core_create_software_context (HS_CORE (self),
                                                   width, height,
                                                   HS_PIXEL_FORMAT_XRGB8888);

  self->core->setVideoBuffer (self->core,
                              hs_software_context_get_framebuffer (self->context),
                              width);

  if (!mCoreLoadFile (self->core, rom_paths[0])) {
    g_set_error (error, HS_CORE_ERROR, HS_CORE_ERROR_COULDNT_LOAD_ROM, "Failed to load ROM");

    return FALSE;
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

  refresh_screen_area (self);
}

static void
mgba_core_reset (HsCore *core)
{
  mGBACore *self = MGBA_CORE (core);

  self->core->reset (self->core);

  refresh_screen_area (self);
}

static void
mgba_core_stop (HsCore *core)
{
  mGBACore *self = MGBA_CORE (core);

  g_clear_object (&self->context);
}

const int GB_BUTTON_MAPPING[] = {
  GB_KEY_UP,
  GB_KEY_DOWN,
  GB_KEY_LEFT,
  GB_KEY_RIGHT,
  GB_KEY_A,
  GB_KEY_B,
  GB_KEY_SELECT,
  GB_KEY_START,
};

const int GBA_BUTTON_MAPPING[] = {
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

  if (is_gba (self)) {
    uint32_t buttons = input_state->game_boy_advance.buttons;

    for (int btn = 0; btn < HS_GAME_BOY_ADVANCE_N_BUTTONS; btn++) {
      if (buttons & 1 << btn)
        keys |= 1 << GBA_BUTTON_MAPPING[btn];
    }
  } else {
    uint32_t buttons = input_state->game_boy.buttons;

    for (int btn = 0; btn < HS_GAME_BOY_N_BUTTONS; btn++) {
      if (buttons & 1 << btn)
        keys |= 1 << GB_BUTTON_MAPPING[btn];
    }
  }

  self->core->setKeys (self->core, keys);
}

static void
mgba_core_run_frame (HsCore *core)
{
  mGBACore *self = MGBA_CORE (core);

  self->core->runFrame (self->core);

  if (is_gba (self)) {
    struct mAudioBuffer *buffer = self->core->getAudioBuffer (self->core);
    int available = mAudioBufferAvailable (buffer);

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

      int produced = mAudioBufferRead (buffer, self->audio_buffer, samples_to_read);

      if (produced > 0)
        hs_core_play_samples (core, self->audio_buffer, produced * 2);
    }
  }
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

  refresh_screen_area (self);

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

  self->core->currentVideoSize (self->core, &width, &height);

  return (double) width / (double) height;
}

static double
mgba_core_get_sample_rate (HsCore *core)
{
  mGBACore *self = MGBA_CORE (core);

  return self->core->audioSampleRate (self->core);
}

static void
postAudioBuffer (struct mAVStream* stream, struct mAudioBuffer* buffer)
{
  int produced;

  UNUSED(stream);

  g_assert (core);

  produced = mAudioBufferRead (buffer, core->audio_buffer, GB_SAMPLES);

  if (produced > 0)
    hs_core_play_samples (HS_CORE (core), core->audio_buffer, produced * 2);
}

static void
mgba_core_constructed (GObject *object)
{
  mGBACore *self = MGBA_CORE (object);

  G_OBJECT_CLASS (mgba_core_parent_class)->constructed (object);

  if (is_gba (self))
    self->core = GBACoreCreate ();
  else if (is_gb (self))
    self->core = GBCoreCreate ();
  else
    g_assert_not_reached ();

  mCoreInitConfig (self->core, NULL);

  struct mCoreOptions opts = {
    .useBios = true,
  };
  mCoreConfigLoadDefaults (&self->core->config, &opts);

  self->logger.log = log_cb;
  mLogSetDefaultLogger(&self->logger);

  mRumbleIntegratorInit (&self->rumble);
  self->rumble.setRumble = set_rumble_cb;

  self->core->init (self->core);

  if (is_gba (self)) {
    size_t samples_per_frame;

    samples_per_frame = mgba_core_get_sample_rate (HS_CORE (self)) / mgba_core_get_frame_rate (HS_CORE (self));

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
  } else {
    self->stream.videoDimensionsChanged = 0;
    self->stream.postAudioFrame = 0;
    self->stream.postAudioBuffer = postAudioBuffer;
    self->stream.postVideoFrame = 0;

    self->audio_buffer_size = GB_SAMPLES * 2;
    self->audio_buffer = malloc (GB_SAMPLES * 2 * sizeof(int16_t));
    self->audio_samples_per_frame_avg = GB_SAMPLES;

    self->core->setAVStream (self->core, &self->stream);
    self->core->setAudioBufferSize (self->core, GB_SAMPLES);
  }

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
mgba_game_boy_core_set_model (HsGameBoyCore *core, HsGameBoyModel model)
{
  mGBACore *self = MGBA_CORE (core);
  enum GBModel mgba_model;
  const char *model_name;

  switch (model) {
  case HS_GAME_BOY_MODEL_GAME_BOY:
    mgba_model = GB_MODEL_DMG;
    break;
  case HS_GAME_BOY_MODEL_GAME_BOY_POCKET:
    mgba_model = GB_MODEL_MGB;
    break;
  case HS_GAME_BOY_MODEL_GAME_BOY_COLOR:
    mgba_model = GB_MODEL_CGB;
    break;
  case HS_GAME_BOY_MODEL_GAME_BOY_ADVANCE:
    mgba_model = GB_MODEL_AGB;
    break;
  case HS_GAME_BOY_MODEL_SUPER_GAME_BOY:
    mgba_model = GB_MODEL_SGB;
    break;
  case HS_GAME_BOY_MODEL_SUPER_GAME_BOY_2:
    mgba_model = GB_MODEL_SGB2;
    break;
  default:
    g_assert_not_reached ();
  }

  model_name = GBModelToName (mgba_model);

  mCoreConfigSetDefaultValue (&self->core->config, "gb.model", model_name);
  mCoreConfigSetDefaultValue (&self->core->config, "sgb.model", model_name);
  mCoreConfigSetDefaultValue (&self->core->config, "cgb.model", model_name);
  mCoreConfigSetDefaultValue (&self->core->config, "cgb.hybridModel", model_name);
  mCoreConfigSetDefaultValue (&self->core->config, "cgb.sgbModel", model_name);
}

static void
mgba_game_boy_core_set_palette (HsGameBoyCore *core, int *colors, int n_colors)
{
  mGBACore *self = MGBA_CORE (core);

  g_assert (n_colors == 4 || n_colors == 12);

  mCoreConfigSetUIntValue (&self->core->config, "gb.pal[0]",  colors[0  % n_colors]);
  mCoreConfigSetUIntValue (&self->core->config, "gb.pal[1]",  colors[1  % n_colors]);
  mCoreConfigSetUIntValue (&self->core->config, "gb.pal[2]",  colors[2  % n_colors]);
  mCoreConfigSetUIntValue (&self->core->config, "gb.pal[3]",  colors[3  % n_colors]);
  mCoreConfigSetUIntValue (&self->core->config, "gb.pal[4]",  colors[4  % n_colors]);
  mCoreConfigSetUIntValue (&self->core->config, "gb.pal[5]",  colors[5  % n_colors]);
  mCoreConfigSetUIntValue (&self->core->config, "gb.pal[6]",  colors[6  % n_colors]);
  mCoreConfigSetUIntValue (&self->core->config, "gb.pal[7]",  colors[7  % n_colors]);
  mCoreConfigSetUIntValue (&self->core->config, "gb.pal[8]",  colors[8  % n_colors]);
  mCoreConfigSetUIntValue (&self->core->config, "gb.pal[9]",  colors[9  % n_colors]);
  mCoreConfigSetUIntValue (&self->core->config, "gb.pal[10]", colors[10 % n_colors]);
  mCoreConfigSetUIntValue (&self->core->config, "gb.pal[11]", colors[11 % n_colors]);

  self->core->reloadConfigOption (self->core, "gb.pal", NULL);
}

static void
mgba_game_boy_core_set_show_sgb_borders (HsGameBoyCore *core, gboolean show_borders)
{
  mGBACore *self = MGBA_CORE (core);

  mCoreConfigSetDefaultIntValue (&self->core->config, "sgb.borders", show_borders ? 1 : 0);

  self->core->reloadConfigOption (self->core, "sgb.borders", NULL);

  refresh_screen_area (self);
}

static void
mgba_game_boy_core_init (HsGameBoyCoreInterface *iface)
{
  iface->set_model = mgba_game_boy_core_set_model;
  iface->set_palette = mgba_game_boy_core_set_palette;
  iface->set_show_sgb_borders = mgba_game_boy_core_set_show_sgb_borders;
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
