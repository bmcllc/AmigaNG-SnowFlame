// Amiga SnowFlame SF-100 — topo do backend SystemVerilog (Machine Profile "sf100", realização em nível de transação).
// Instancia os dispositivos do SoC; o frontend em C++ (fabric/) liga cada instância ao hospedeiro (<dev>_bind) e chama a
// API exportada de cada uma no escopo dela (svSetScope). A interconexão, as memórias, o relógio e as mídias são do Host Fabric.
module sf100;
  sf_cpu      cpu();         // SnowFire-CF266 (ColdFire V4e)
  sf_bridge   bridge();
  sf_vpu      vpuA();
  sf_vpu      vpuB();
  sf_flamecore gpu();
  sf_blitter  blt();
  sf_display  display();     // SnowCopper + vídeo
  sf_paula    paula();
  sf_timers   timers();
  sf_dvd      dvd();
  sf_mcard    mcard();
  sf_pads     pads();
  sf_mmu      mmu();
  sf_hid      hid();
  sf_dbgcon   dbg();
endmodule
