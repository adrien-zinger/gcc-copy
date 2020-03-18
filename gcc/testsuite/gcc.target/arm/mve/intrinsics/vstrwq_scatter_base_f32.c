/* { dg-do compile  } */
/* { dg-require-effective-target arm_v8_1m_mve_fp_ok } */
/* { dg-add-options arm_v8_1m_mve_fp } */
/* { dg-additional-options "-O2" } */

#include "arm_mve.h"

void
foo (uint32x4_t addr, float32x4_t value)
{
  vstrwq_scatter_base_f32 (addr, 8, value);
}

/* { dg-final { scan-assembler "vstrw.u32"  }  } */

void
foo1 (uint32x4_t addr, float32x4_t value)
{
  vstrwq_scatter_base (addr, 8, value);
}

/* { dg-final { scan-assembler "vstrw.u32"  }  } */
