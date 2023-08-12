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
  gpointer output_buffer;
  int16_t *audio_buffer;

  struct mStandardLogger logger;

  HsGameBoyModel model;
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
  HsPlatform platform = hs_core_get_platform (HS_CORE (self));

  platform = hs_platform_get_base_platform (platform);

  return platform == HS_PLATFORM_GAME_BOY;
}

static void
switch_to_specific_model (mGBACore *self, enum GBModel model)
{
  enum GBColorLookup colors;
  const char *model_name;

  switch (model) {
  case GB_MODEL_DMG:
  case GB_MODEL_MGB:
    colors = GB_COLORS_NONE;
    break;
  case GB_MODEL_CGB:
  case GB_MODEL_AGB:
    colors = GB_COLORS_CGB;
    break;
  case GB_MODEL_SGB:
  case GB_MODEL_SGB2:
    colors = GB_COLORS_SGB;
    break;
  case GB_MODEL_AUTODETECT:
  default:
    g_assert_not_reached ();
  }

  model_name = GBModelToName (model);

  mCoreConfigSetDefaultValue (&self->core->config, "gb.model", model_name);
  mCoreConfigSetDefaultValue (&self->core->config, "sgb.model", model_name);
  mCoreConfigSetDefaultValue (&self->core->config, "cgb.model", model_name);
  mCoreConfigSetDefaultValue (&self->core->config, "cgb.hybridModel", model_name);
  mCoreConfigSetDefaultValue (&self->core->config, "cgb.sgbModel", model_name);

  mCoreConfigSetDefaultIntValue (&self->core->config, "gb.colors", colors);
}

static void
sync_model (mGBACore *self)
{
  if (self->model == HS_GAME_BOY_MODEL_AUTO_PREFER_GBC ||
      self->model == HS_GAME_BOY_MODEL_AUTO_PREFER_SGB) {
    struct GBCartridgeOverride override;
    struct GB *gb = (struct GB *) self->core->board;
    bool has_gbc_override, has_sgb_override;
    bool gbc_enhanced = false, sgb_enhanced = false;
    int valid_models;

    if (!gb->memory.rom || gb->memory.romSize < sizeof (struct GBCartridge) + 0x100) {
      switch_to_specific_model (self, GB_MODEL_DMG);
      return;
    }

    valid_models = GBValidModels(gb->memory.rom);

    switch (valid_models) {
    case GB_MODEL_SGB | GB_MODEL_MGB:
      sgb_enhanced = true;
      break;
    case GB_MODEL_SGB | GB_MODEL_CGB: // TODO: Do these even exist?
    case GB_MODEL_MGB | GB_MODEL_SGB | GB_MODEL_CGB:
      sgb_enhanced = gbc_enhanced = true;
      break;
    case GB_MODEL_CGB:
    case GB_MODEL_MGB | GB_MODEL_CGB:
      gbc_enhanced = true;
      break;
    default:
      break;
    }

    override.headerCrc32 = doCrc32 (&gb->memory.rom[0x100], sizeof (struct GBCartridge));
    has_gbc_override = GBOverrideColorFind (&override, GB_COLORS_CGB);
    has_sgb_override = GBOverrideColorFind (&override, GB_COLORS_SGB);

    if ((sgb_enhanced && gbc_enhanced) || (has_sgb_override && has_gbc_override)) {
      if (self->model == HS_GAME_BOY_MODEL_AUTO_PREFER_GBC)
        switch_to_specific_model (self, GB_MODEL_CGB);
      else
        switch_to_specific_model (self, GB_MODEL_SGB);

      return;
    }

    if (sgb_enhanced || (has_sgb_override && !gbc_enhanced)) {
      switch_to_specific_model (self, GB_MODEL_SGB);
      return;
    }

    if (gbc_enhanced || has_gbc_override) {
      switch_to_specific_model (self, GB_MODEL_CGB);
      return;
    }

    switch_to_specific_model (self, GB_MODEL_DMG);
  } else {
    enum GBModel model;

    switch (self->model) {
    case HS_GAME_BOY_MODEL_GAME_BOY:
      model = GB_MODEL_DMG;
      break;
    case HS_GAME_BOY_MODEL_GAME_BOY_COLOR:
      model = GB_MODEL_CGB;
      break;
    case HS_GAME_BOY_MODEL_GAME_BOY_ADVANCE:
      model = GB_MODEL_AGB;
      break;
    case HS_GAME_BOY_MODEL_SUPER_GAME_BOY:
      model = GB_MODEL_SGB;
      break;
    case HS_GAME_BOY_MODEL_AUTO_PREFER_GBC:
    case HS_GAME_BOY_MODEL_AUTO_PREFER_SGB:
    default:
      g_assert_not_reached ();
    }

    switch_to_specific_model (self, model);
  }
}

static gboolean
mgba_core_start (HsCore      *core,
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

  self->output_buffer = hs_core_request_framebuffer (HS_CORE (self),
                                                     width, height,
                                                     HS_PIXEL_FORMAT_XRGB8888);

  self->core->setVideoBuffer (self->core, self->output_buffer, width);

  if (!mCoreLoadFile (self->core, rom_path)) {
    g_set_error (error, HS_CORE_ERROR, HS_CORE_ERROR_COULDNT_LOAD_ROM, "Failed to load ROM");

    return FALSE;
  }

  if (is_gb (self))
    sync_model (self);

  self->core->reset (self->core);

  mCoreLoadSaveFile (self->core, save_path, FALSE);

  return TRUE;
}

static void
mgba_core_reset (HsCore *core)
{
  mGBACore *self = MGBA_CORE (core);

  self->core->reset (self->core);
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

static gboolean
mgba_core_load_state (HsCore      *core,
                      const char  *path,
                      GError     **error)
{
  mGBACore *self = MGBA_CORE (core);
  struct VFile* vf = VFileOpen (path, O_RDONLY);

  mCoreLoadStateNamed (self->core, vf, SAVESTATE_SAVEDATA | SAVESTATE_RTC);

  vf->close (vf);

  return TRUE;
}

static gboolean
mgba_core_save_state (HsCore      *core,
                      const char  *path,
                      GError     **error)
{
  mGBACore *self = MGBA_CORE (core);
  struct VFile* vf = VFileOpen (path, O_CREAT | O_TRUNC | O_RDWR);

  mCoreSaveStateNamed (self->core, vf, SAVESTATE_SAVEDATA | SAVESTATE_RTC);

  vf->close (vf);

  return TRUE;
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

static gboolean
mgba_core_get_screen_rect (HsCore      *core,
                           HsRectangle *rect)
{
  mGBACore *self = MGBA_CORE (core);
  unsigned width, height;

  self->core->currentVideoSize (self->core, &width, &height);

  hs_rectangle_init (rect, 0, 0, width, height);

  return TRUE;
}

static size_t
mgba_core_get_row_stride (HsCore *core)
{
  mGBACore *self = MGBA_CORE (core);
  unsigned width, height;

  self->core->baseVideoSize (self->core, &width, &height);

  return width * BYTES_PER_PIXEL;
}

static double
mgba_core_get_sample_rate (HsCore *core)
{
  return SAMPLE_RATE;
}

static void
postAudioBuffer (struct mAVStream* stream, blip_t* left, blip_t* right) {
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
    g_error ("Unsupported platform");

  mCoreInitConfig (self->core, NULL);

  struct mCoreOptions opts = {
    .useBios = true,
  };
  mCoreConfigLoadDefaults (&self->core->config, &opts);

  mCoreConfigSetDefaultIntValue (&self->core->config, "logToStdout", false);

  mStandardLoggerInit (&self->logger);
  mStandardLoggerConfig (&self->logger, &self->core->config);
  mLogSetDefaultLogger (&self->logger.d);

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

  mStandardLoggerDeinit (&self->logger);
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

  core_class->start = mgba_core_start;
  core_class->reset = mgba_core_reset;
  core_class->run_frame = mgba_core_run_frame;

  core_class->load_state = mgba_core_load_state;
  core_class->save_state = mgba_core_save_state;

  core_class->get_frame_rate = mgba_core_get_frame_rate;
  core_class->get_aspect_ratio = mgba_core_get_aspect_ratio;
  core_class->get_screen_rect = mgba_core_get_screen_rect;
  core_class->get_row_stride = mgba_core_get_row_stride;

  core_class->get_sample_rate = mgba_core_get_sample_rate;
}

static void
mgba_core_init (mGBACore *self)
{
  g_assert (!core);

  core = self;
}

const int gb_button_mapping[] = {
  GB_KEY_UP,
  GB_KEY_DOWN,
  GB_KEY_LEFT,
  GB_KEY_RIGHT,
  GB_KEY_A,
  GB_KEY_B,
  GB_KEY_SELECT,
  GB_KEY_START,
};

static void
mgba_game_boy_core_button_pressed (HsGameBoyCore *core, HsGameBoyButton button)
{
  mGBACore *self = MGBA_CORE (core);

  self->core->addKeys(self->core, 1 << gb_button_mapping[button]);
}

static void
mgba_game_boy_core_button_released (HsGameBoyCore *core, HsGameBoyButton button)
{
  mGBACore *self = MGBA_CORE (core);

  self->core->clearKeys(self->core, 1 << gb_button_mapping[button]);
}

static void
mgba_game_boy_core_set_model (HsGameBoyCore *core, HsGameBoyModel model)
{
  mGBACore *self = MGBA_CORE (core);

  self->model = model;
}

static void
mgba_game_boy_core_init (HsGameBoyCoreInterface *iface)
{
  iface->button_pressed = mgba_game_boy_core_button_pressed;
  iface->button_released = mgba_game_boy_core_button_released;

  iface->set_model = mgba_game_boy_core_set_model;
}

const int gba_button_mapping[] = {
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
mgba_game_boy_advance_core_button_pressed (HsGameBoyAdvanceCore *core, HsGameBoyAdvanceButton button)
{
  mGBACore *self = MGBA_CORE (core);

  self->core->addKeys(self->core, 1 << gba_button_mapping[button]);
}

static void
mgba_game_boy_advance_core_button_released (HsGameBoyAdvanceCore *core, HsGameBoyAdvanceButton button)
{
  mGBACore *self = MGBA_CORE (core);

  self->core->clearKeys(self->core, 1 << gba_button_mapping[button]);
}

static void
mgba_game_boy_advance_core_init (HsGameBoyAdvanceCoreInterface *iface)
{
  iface->button_pressed = mgba_game_boy_advance_core_button_pressed;
  iface->button_released = mgba_game_boy_advance_core_button_released;
}

GType
hs_get_core_type (void)
{
  return MGBA_TYPE_CORE;
}
