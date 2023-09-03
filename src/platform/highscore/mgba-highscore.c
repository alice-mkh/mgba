#include "mgba-highscore.h"

#include <mgba/core/blip_buf.h>
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
#define GBA_SAMPLES 1024
#define SAMPLE_RATE 32768

static mGBACore *core;

struct _mGBACore
{
  HsCore parent_instance;

  struct mCore *core;
  struct mAVStream stream;
  HsSoftwareContext *context;
  int16_t *audio_buffer;

  struct mLogger logger;
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
log_cb (struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
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


static gboolean
mgba_core_load_rom (HsCore      *core,
                    const char  *rom_path,
                    const char  *save_path,
                    GError     **error)
{
  mGBACore *self = MGBA_CORE (core);

  blip_set_rates (self->core->getAudioChannel (self->core, 0),
                  self->core->frequency (self->core), SAMPLE_RATE);
  blip_set_rates (self->core->getAudioChannel (self->core, 1),
                  self->core->frequency (self->core), SAMPLE_RATE);

  unsigned width, height;

  self->core->baseVideoSize (self->core, &width, &height);

  self->context = hs_core_create_software_context (HS_CORE (self),
                                                   width, height,
                                                   HS_PIXEL_FORMAT_XRGB8888);

  self->core->setVideoBuffer (self->core,
                              hs_software_context_get_framebuffer (self->context),
                              width);

  if (!mCoreLoadFile (self->core, rom_path)) {
    g_set_error (error, HS_CORE_ERROR, HS_CORE_ERROR_COULDNT_LOAD_ROM, "Failed to load ROM");

    return FALSE;
  }

  self->core->reset (self->core);

  mCoreLoadSaveFile (self->core, save_path, FALSE);

  refresh_screen_area (self);

  return TRUE;
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
    gint16 samples[GBA_SAMPLES * 2];
    gsize available = 0;

    available = blip_samples_avail (self->core->getAudioChannel (self->core, 0));
    blip_read_samples (self->core->getAudioChannel (self->core, 0), samples, available, true);
    blip_read_samples (self->core->getAudioChannel (self->core, 1), samples + 1, available, true);

    hs_core_play_samples (core, samples, available * 2);
  }
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
  return SAMPLE_RATE;
}

static void
postAudioBuffer (struct mAVStream* stream, blip_t* left, blip_t* right)
{
  int produced;

  UNUSED(stream);

  g_assert (core);

  produced = blip_read_samples (left, core->audio_buffer, GB_SAMPLES, true);
  produced += blip_read_samples (right, core->audio_buffer + 1, GB_SAMPLES, true);

  if (produced > 0)
    hs_core_play_samples (HS_CORE (core), core->audio_buffer, produced);
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

  self->core->init (self->core);

  if (is_gba (self)) {
    self->core->setAudioBufferSize (self->core, GBA_SAMPLES);
  } else {
    self->stream.videoDimensionsChanged = 0;
    self->stream.postAudioFrame = 0;
    self->stream.postAudioBuffer = postAudioBuffer;
    self->stream.postVideoFrame = 0;

    self->audio_buffer = malloc (GB_SAMPLES * 2 * sizeof(int16_t));

    self->core->setAVStream (self->core, &self->stream);
    self->core->setAudioBufferSize (self->core, GB_SAMPLES);
  }
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
  core_class->reset = mgba_core_reset;
  core_class->stop = mgba_core_stop;
  core_class->poll_input = mgba_core_poll_input;
  core_class->run_frame = mgba_core_run_frame;

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
}

GType
hs_get_core_type (void)
{
  return MGBA_TYPE_CORE;
}
