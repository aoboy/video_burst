#include "burst/adaptation_engine.h"

#include "test_framework.h"

using bursts::AdaptationEngine;
using bursts::StreamState;

TEST(AdaptationNoReductionIsNoOp) {
  StreamState s;
  s.adaptation_level = 1;
  s.crf_offset = 3.0;
  AdaptationEngine e;
  e.Apply(s, 0.05);
  EXPECT_EQ(s.adaptation_level, 0);
  EXPECT_LT(s.crf_offset, 3.0);  // gradual restore applied
}

TEST(AdaptationStage1Crf) {
  StreamState s;
  AdaptationEngine e;
  e.Apply(s, 0.15);
  EXPECT_EQ(s.adaptation_level, 1);
  EXPECT_NEAR(s.crf_offset, std::min(12.0, 0.15 * 60.0), 1e-9);
  EXPECT_EQ(s.resolution_scale, 1);
  EXPECT_FALSE(s.drop_b_frames);
  EXPECT_FALSE(s.drop_p_frames);
}

TEST(AdaptationStage1CrfCapsAt12) {
  StreamState s;
  AdaptationEngine e;
  e.Apply(s, 0.19);
  EXPECT_EQ(s.adaptation_level, 1);
  EXPECT_NEAR(s.crf_offset, 0.19 * 60.0, 1e-9);
  EXPECT_LE(s.crf_offset, 12.0);
}

TEST(AdaptationStage2Resolution) {
  StreamState s;
  AdaptationEngine e;
  e.Apply(s, 0.45);
  EXPECT_EQ(s.adaptation_level, 2);
  EXPECT_EQ(s.resolution_scale, 2);
  EXPECT_NEAR(s.crf_offset, 6.0, 1e-9);
  EXPECT_FALSE(s.drop_b_frames);
  EXPECT_FALSE(s.drop_p_frames);
}

TEST(AdaptationStage3Temporal) {
  StreamState s;
  AdaptationEngine e;
  e.Apply(s, 0.60);
  EXPECT_EQ(s.adaptation_level, 3);
  EXPECT_EQ(s.resolution_scale, 2);
  EXPECT_TRUE(s.drop_b_frames);
  EXPECT_FALSE(s.drop_p_frames);
}

TEST(AdaptationStage4Shed) {
  StreamState s;
  AdaptationEngine e;
  e.Apply(s, 0.90);
  EXPECT_EQ(s.adaptation_level, 4);
  EXPECT_EQ(s.resolution_scale, 4);
  EXPECT_TRUE(s.drop_b_frames);
  EXPECT_TRUE(s.drop_p_frames);
  EXPECT_NEAR(s.crf_offset, 12.0, 1e-9);
}

TEST(AdaptationBoundaryAtExact03) {
  StreamState s;
  AdaptationEngine e;
  e.Apply(s, 0.30);
  EXPECT_EQ(s.adaptation_level, 2);  // 0.3 < 0.5 -> stage 2
}

TEST(AdaptationRestoreFullyResets) {
  StreamState s;
  AdaptationEngine e;
  e.Apply(s, 0.90);  // to stage 4

  // Python-faithful restore semantics: `_restore` is only invoked while the
  // stream's adaptation_level is still > 0, so a single tick drops the level
  // to 0 and shaves CRF by 0.5. Quality parameters then stay until the next
  // burst triggers adaptation again.
  e.Apply(s, 0.05);  // demand ends
  EXPECT_EQ(s.adaptation_level, 0);
  EXPECT_NEAR(s.crf_offset, 12.0 - 0.5, 1e-9);
  // Codec parameters intentionally persist (they are restored only inside
  // _restore while the level is nonzero).
  EXPECT_EQ(s.resolution_scale, 4);
  EXPECT_TRUE(s.drop_b_frames);
  EXPECT_TRUE(s.drop_p_frames);
}