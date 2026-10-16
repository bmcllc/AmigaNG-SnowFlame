#!/bin/bash
# modules.sh - lista de fontes do ioquake3 compiladas para o SF-100 (cada módulo com seus defines) e função de compilação com o GCC cross.
IOQ3=${IOQ3:-"$(cd "$(dirname "$0")/../../.." && pwd)/Games-To-Port/ioq3"}
HERE=$(cd "$(dirname "$0")/.." && pwd)
if [ -n "$SF_M68K_CROSS" ]; then export PATH="$SF_M68K_CROSS/usr/bin:$PATH"; export LD_LIBRARY_PATH="$SF_M68K_CROSS/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH"; fi
CC=${CC:-m68k-linux-gnu-gcc-14}
GCCINC=$($CC -print-file-name=include)   # cabeçalhos freestanding do GCC (stddef, stdarg, stdint, float, limits); /usr/include do host fica de fora
CODE="$IOQ3/code"
CFLAGS_COMMON=(-mcpu=5475 -O2 -g0 -ffreestanding -nostdinc "-isystem$GCCINC" -fno-builtin-printf -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -mno-pcrel
  "-I$HERE/rt/include" "-I$HERE/ioq3/include" "-I$CODE/qcommon" "-I$CODE/client" "-I$CODE/server" "-I$CODE/renderercommon" "-I$CODE/botlib" "-I$CODE/thirdparty/zlib-1.3.1" "-I$CODE/renderercommon" "-I$CODE/thirdparty/jpeg-9f" "-I$HERE/ioq3/sfr"
  -Wno-misleading-indentation -Wno-stringop-overflow -Wno-array-bounds -w
  '-DPRODUCT_VERSION="1.36_sf100"' '-DARCH_STRING="m68k"' -DBOTLIB -DSTANDALONE '-DBASEGAME="baseoa"' -DDEF_COMZONEMEGS=12 -DDEF_COMHUNKMEGS=32 -DMAX_RAW_STREAMS=3 -DUSE_INTERNAL_ZLIB -DSF100 -DNO_VM_COMPILED)
# grupos de fontes (relativos a code/)
QCOMMON=(qcommon/cm_load.c qcommon/cm_patch.c qcommon/cm_polylib.c qcommon/cm_test.c qcommon/cm_trace.c qcommon/cmd.c qcommon/common.c qcommon/cvar.c qcommon/files.c
  qcommon/huffman.c qcommon/md4.c qcommon/md5.c qcommon/msg.c qcommon/net_chan.c qcommon/q_math.c qcommon/q_shared.c qcommon/vm.c qcommon/vm_interpreted.c qcommon/ioapi.c qcommon/unzip.c)
SERVER=(server/sv_bot.c server/sv_ccmds.c server/sv_client.c server/sv_game.c server/sv_init.c server/sv_main.c server/sv_net_chan.c server/sv_snapshot.c server/sv_world.c)
BOTLIB=($(cd "$CODE" && ls botlib/*.c))
CLIENT=(client/cl_cgame.c client/cl_console.c client/cl_input.c client/cl_keys.c client/cl_main.c client/cl_net_chan.c client/cl_parse.c client/cl_scrn.c client/cl_ui.c client/cl_avi.c client/cl_cin.c
  client/snd_adpcm.c client/snd_codec.c client/snd_codec_wav.c client/snd_dma.c client/snd_main.c client/snd_mem.c client/snd_mix.c client/snd_wavelet.c)
GAME=(game/ai_chat.c game/ai_cmd.c game/ai_dmnet.c game/ai_dmq3.c game/ai_main.c game/ai_team.c game/ai_vcmd.c game/bg_lib.c game/bg_misc.c game/bg_pmove.c game/bg_slidemove.c
  game/g_active.c game/g_arenas.c game/g_bot.c game/g_client.c game/g_cmds.c game/g_combat.c game/g_items.c game/g_main.c game/g_mem.c game/g_misc.c game/g_missile.c game/g_mover.c
  game/g_session.c game/g_spawn.c game/g_svcmds.c game/g_syscalls.c game/g_target.c game/g_team.c game/g_trigger.c game/g_utils.c game/g_weapon.c qcommon/q_math.c qcommon/q_shared.c)
CGAME=($(cd "$CODE" && ls cgame/*.c | grep -v -e cg_newdraw.c) game/bg_lib.c game/bg_misc.c game/bg_pmove.c game/bg_slidemove.c qcommon/q_math.c qcommon/q_shared.c)
UI=($(cd "$CODE" && ls q3_ui/*.c | grep -v -e ui_login.c -e ui_rankings.c -e ui_rankstatus.c -e ui_signup.c -e ui_specifyleague.c) game/bg_lib.c game/bg_misc.c qcommon/q_math.c qcommon/q_shared.c)
ZLIB=(thirdparty/zlib-1.3.1/adler32.c thirdparty/zlib-1.3.1/crc32.c thirdparty/zlib-1.3.1/inflate.c thirdparty/zlib-1.3.1/inftrees.c thirdparty/zlib-1.3.1/inffast.c thirdparty/zlib-1.3.1/zutil.c)
# renderer SF-100 (sfr/), carregadores de imagem do renderercommon e libjpeg (decodificador)
SFR=(sfr/sfr_image.c sfr/sfr_shader.c sfr/sfr_backend.c sfr/sfr_world.c sfr/sfr_model.c sfr/sfr_scene.c sfr/sfr_init.c sfr/jpeg_stubs.c)
RCOMMON=(renderercommon/tr_image_tga.c renderercommon/tr_image_jpg.c renderercommon/tr_image_png.c renderercommon/tr_image_bmp.c renderercommon/tr_image_pcx.c renderercommon/puff.c renderercommon/tr_font.c)
JPEG=(jaricom jcomapi jdapimin jdapistd jdarith jdatasrc jdcoefct jdcolor jddctmgr jdhuff jdinput jdmainct jdmarker jdmaster jdmerge jdpostct jdsample jdtrans jerror jidctflt jidctfst jidctint jmemmgr jmemnobs jquant1 jquant2 jutils)
# compile_one grupo_de_defines arquivo.c saida.o
compile_one() { local defs=$1 src=$2 out=$3; $CC "${CFLAGS_COMMON[@]}" $defs -c "$CODE/$src" -o "$out"; }
