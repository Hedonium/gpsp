

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "common.h"
#include "libco.h"
#include "libretro.h"

#ifndef MAX_PATH
#define MAX_PATH (512)
#endif

static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_environment_t environ_cb;

struct retro_perf_callback perf_cb;

static cothread_t main_thread;
static cothread_t cpu_thread;
int dynarec_enable;

u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 iwram_stack_optimize = 1;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets = 0;

void switch_to_main_thread(void)
{
   co_switch(main_thread);
}

static inline void switch_to_cpu_thread(void)
{
   co_switch(cpu_thread);
}

static void cpu_thread_entry(void)
{
#ifdef HAVE_DYNAREC
   if (dynarec_enable)
      execute_arm_translate(execute_cycles);
#endif
   execute_arm(execute_cycles);
}

static inline void init_context_switch(void)
{
   main_thread = co_active();
   cpu_thread = co_create(0x20000, cpu_thread_entry);
}

static inline void deinit_context_switch(void)
{
   co_delete(cpu_thread);
}

#ifdef PERF_TEST

extern struct retro_perf_callback perf_cb;

#define RETRO_PERFORMANCE_INIT(X) \
   static struct retro_perf_counter X = {#X}; \
   do { \
      if (!(X).registered) \
         perf_cb.perf_register(&(X)); \
   } while(0)

#define RETRO_PERFORMANCE_START(X) perf_cb.perf_start(&(X))
#define RETRO_PERFORMANCE_STOP(X) perf_cb.perf_stop(&(X))
#else
#define RETRO_PERFORMANCE_INIT(X)
#define RETRO_PERFORMANCE_START(X)
#define RETRO_PERFORMANCE_STOP(X)

#endif

void retro_get_system_info(struct retro_system_info* info)
{
   info->library_name = "gpSP";
   info->library_version = "v0.91";
   info->need_fullpath = true;
   info->block_extract = false;
   info->valid_extensions = "gba|bin|agb|gbz" ;
}


void retro_get_system_av_info(struct retro_system_av_info* info)
{
   info->geometry.base_width = GBA_SCREEN_WIDTH;
   info->geometry.base_height = GBA_SCREEN_HEIGHT;
   info->geometry.max_width = GBA_SCREEN_WIDTH;
   info->geometry.max_height = GBA_SCREEN_HEIGHT;
   info->geometry.aspect_ratio = 0;
   // 59.72750057 hz
   info->timing.fps = ((float) GBC_BASE_RATE) / (308 * 228 * 4);
   info->timing.sample_rate = GBA_SOUND_FREQUENCY;
}

#include <sys/mman.h>

void retro_init(void)
{
   init_gamepak_buffer();
   init_sound(1);

}

void retro_deinit(void)
{
   perf_cb.perf_log();
   memory_term();

#if defined(HAVE_MMAP) && defined(HAVE_DYNAREC)
   munmap(rom_translation_cache, ROM_TRANSLATION_CACHE_SIZE);
   munmap(ram_translation_cache, RAM_TRANSLATION_CACHE_SIZE);
   munmap(bios_translation_cache, BIOS_TRANSLATION_CACHE_SIZE);
#endif
}

void retro_set_environment(retro_environment_t cb)
{
   struct retro_log_callback log;

   static const struct retro_variable vars[] = {
#ifdef HAVE_DYNAREC
      { "gpsp_drc", "Dynamic recompiler (restart); enabled|disabled" },
#endif
      { NULL, NULL },
   };

   environ_cb = cb;

   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

   environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf_cb);
   environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}
void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {}

void retro_reset(void)
{
   deinit_context_switch();

   update_backup();
   reset_gba();

   init_context_switch();
}


size_t retro_serialize_size(void)
{
   return GBA_STATE_MEM_SIZE;
}

bool retro_serialize(void* data, size_t size)
{
   if (size != GBA_STATE_MEM_SIZE)
      return false;

   memset (data,0, GBA_STATE_MEM_SIZE);
   gba_save_state(data);

   return true;
}

bool retro_unserialize(const void* data, size_t size)
{
   if (size != GBA_STATE_MEM_SIZE)
      return false;

   gba_load_state(data);

   return true;
}

void retro_cheat_reset(void)
{
}
void retro_cheat_set(unsigned index, bool enabled, const char* code) {}

void error_msg(const char* text)
{
   if (log_cb)
      log_cb(RETRO_LOG_ERROR, "[gpSP]: %s\n", text);
}

void info_msg(const char* text)
{
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "[gpSP]: %s\n", text);
}

static void extract_directory(char* buf, const char* path, size_t size)
{
   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   char* base = strrchr(buf, '/');

   if (base)
      *base = '\0';
   else
      strncpy(buf, ".", size);
}

static void check_variables(int started_from_load)
{
   struct retro_variable var;

#ifdef HAVE_DYNAREC
   var.key = "gpsp_drc";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (started_from_load)
      {
         if (strcmp(var.value, "disabled") == 0)
            dynarec_enable = 0;
         else if (strcmp(var.value, "enabled") == 0)
            dynarec_enable = 1;
      }
   }
   else
      dynarec_enable = 1;
#endif
}

bool retro_load_game(const struct retro_game_info* info)
{
   check_variables(1);

#if defined(HAVE_DYNAREC)
   if (dynarec_enable)
   {
#if defined(HAVE_MMAP)

   rom_translation_cache = mmap(NULL, ROM_TRANSLATION_CACHE_SIZE,
                                PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
   ram_translation_cache = mmap(NULL, RAM_TRANSLATION_CACHE_SIZE,
                                PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
   bios_translation_cache = mmap(NULL, BIOS_TRANSLATION_CACHE_SIZE,
                                 PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);

   rom_translation_ptr = rom_translation_cache;
   ram_translation_ptr = ram_translation_cache;
   bios_translation_ptr = bios_translation_cache;
#endif
   }
   else
      dynarec_enable = 0;
#else
   dynarec_enable = 0;
#endif

   char filename_bios[MAX_PATH];
   const char* dir = NULL;

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
      info_msg("RGB565 is not supported.");

   extract_directory(main_path, info->path, sizeof(main_path));

   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
      strncpy(filename_bios, dir, sizeof(filename_bios));
   else
      strncpy(filename_bios, main_path, sizeof(filename_bios));

   strncat(filename_bios, "/gba_bios.bin", sizeof(filename_bios));


   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
      strncpy(save_path, dir, sizeof(save_path));
   else
      strncpy(save_path, main_path, sizeof(save_path));

   if (load_bios(filename_bios) != 0)
   {
      error_msg("Could not load BIOS image file.");
      return false;
   }

   if (bios_rom[0] != 0x18)
   {
      info_msg("You have an incorrect BIOS image.");
      info_msg("While many games will work fine, some will not.");
      info_msg("It is strongly recommended that you obtain the correct BIOS file.");
   }

   gamepak_filename[0] = 0;

   if (load_gamepak(info->path) != 0)
   {
      error_msg("Could not load the game file.");
      return false;
   }

   reset_gba();

   init_context_switch();

   return true;
}


bool retro_load_game_special(unsigned game_type,
                             const struct retro_game_info* info, size_t num_info)
{
   return false;
}

void retro_unload_game(void)
{
   deinit_context_switch();
   update_backup();
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

void* retro_get_memory_data(unsigned id)
{
   //   switch (id)
   //   {
   //   case RETRO_MEMORY_SAVE_RAM:
   //      return gamepak_backup;
   //   }

   return 0;
}

size_t retro_get_memory_size(unsigned id)
{
   //   switch (id)
   //   {
   //   case RETRO_MEMORY_SAVE_RAM:
   //      switch(backup_type)
   //      {
   //      case BACKUP_SRAM:
   //         return sram_size;

   //      case BACKUP_FLASH:
   //         return flash_size;

   //      case BACKUP_EEPROM:
   //         return eeprom_size;

   //      case BACKUP_NONE:
   //         return 0x0;

   //      default:
   //         return 0x8000;
   //      }
   //   }

   return 0;
}

void retro_run(void)
{
   bool updated = false;

   input_poll_cb();

   switch_to_cpu_thread();

   update_input();

   render_audio();

   video_cb(gba_screen_pixels, GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT,
            GBA_SCREEN_PITCH * 2);

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      check_variables(0);

}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}
