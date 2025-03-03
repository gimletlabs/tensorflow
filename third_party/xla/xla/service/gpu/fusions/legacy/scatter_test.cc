/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "xla/service/gpu/fusions/legacy/scatter.h"

#include <optional>
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "mlir/IR/MLIRContext.h"
#include "mlir/Support/LLVM.h"
#include "xla/service/gpu/fusions/fusions.h"
#include "xla/service/gpu/gpu_device_info_for_tests.h"
#include "xla/service/gpu/hlo_fusion_analysis.h"
#include "xla/service/gpu/model/indexing_map_serialization.h"
#include "xla/service/gpu/model/indexing_test_utils.h"
#include "xla/stream_executor/device_description.h"
#include "xla/tests/hlo_test_base.h"
#include "xla/xla.pb.h"
#include "tsl/platform/statusor.h"

namespace xla {
namespace gpu {
namespace {

class ScatterFusionTest : public HloTestBase {
  DebugOptions GetDebugOptionsForTest() override {
    auto opts = HloTestBase::GetDebugOptionsForTest();
    opts.set_xla_gpu_mlir_emitter_level(0);
    return opts;
  }

 protected:
  mlir::MLIRContext mlir_context_;
};

TEST_F(ScatterFusionTest, ScatterFusion) {
  auto module = ParseAndReturnVerifiedModule(R"(
    HloModule module

    add (lhs: f32[], rhs: f32[]) -> f32[] {
      lhs = f32[] parameter(0)
      rhs = f32[] parameter(1)
      ROOT sum = f32[] add(lhs, rhs)
    }

    fused_computation {
      %input = f32[2,9] parameter(0)
      %indices = s32[3] parameter(1)
      %updates = f32[3,9] parameter(2)
      ROOT %scatter = f32[2,9] scatter(%input, %indices, %updates),
          to_apply=add,
          update_window_dims={1},
          inserted_window_dims={0},
          scatter_dims_to_operand_dims={0},
          index_vector_dim=1
    }

    ENTRY entry {
      %input = f32[2,9] parameter(0)
      %indices = s32[3] parameter(1)
      %updates = f32[3,9] parameter(2)
      ROOT %fusion = f32[2,9] fusion(%input, %indices, %updates), kind=kLoop, calls=fused_computation
    })")
                    .value();

  stream_executor::DeviceDescription device_info =
      TestGpuDeviceInfo::RTXA6000DeviceInfo();

  auto* root = module->entry_computation()->root_instruction();
  auto analysis_fused = HloFusionAnalysis::Create(*root, device_info);

  auto emitter =
      GetFusionEmitter(PreBufferAssignmentFusionInfo{analysis_fused});
  auto scatter_fusion = dynamic_cast<ScatterFusion*>(emitter.get());
  ASSERT_NE(scatter_fusion, nullptr);
  EXPECT_EQ(scatter_fusion->launch_dimensions().launch_bound(),
            3 * 9 /* updates size */);
}

TEST_F(ScatterFusionTest, ThreadIdIndexing) {
  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(R"(
    HloModule module

    computation {
      %p0 = f32[] parameter(0)
      %p1 = f32[] parameter(1)
      %p2 = f32[] parameter(2)
      %p3 = f32[] parameter(3)
      ROOT %tuple = (f32[], f32[]) tuple(f32[] %p2, f32[] %p3)
    }
    scatter {
      %operand0 = f32[300,200] parameter(0)
      %operand1 = f32[300,200] parameter(1)
      %indices = s32[42,1] parameter(2)
      %update.1 = f32[42,10,20] parameter(3)
      %update.2 = f32[42,10,20]parameter(4)

      ROOT %scatter = (f32[300,200], f32[300,200]) scatter(
          f32[300,200] %operand0,
          f32[300,200] %operand1,
          s32[42,1] %indices,
          f32[42,10,20] %update.1,
          f32[42,10,20] %update.2
        ),
        update_window_dims={1,2},
        inserted_window_dims={},
        scatter_dims_to_operand_dims={0},
        index_vector_dim=1,
        to_apply=computation
    }
    ENTRY entry {
      %operand0 = f32[300,200] parameter(0)
      %operand1 = f32[300,200] parameter(1)
      %indices = s32[42,1] parameter(2)
      %update.1 = f32[42,10,20] parameter(3)
      %update.2 = f32[42,10,20]parameter(4)
      ROOT %fusion = (f32[300,200], f32[300,200]) fusion(
        %operand0, %operand1, %indices, %update.1, %update.2),
        kind=kLoop, calls=scatter
    }
  )"));
  stream_executor::DeviceDescription device_info =
      TestGpuDeviceInfo::RTXA6000DeviceInfo();

  auto* root = module->entry_computation()->root_instruction();
  auto analysis_fused = HloFusionAnalysis::Create(*root, device_info);

  auto emitter =
      GetFusionEmitter(PreBufferAssignmentFusionInfo{analysis_fused});
  auto fusion = dynamic_cast<ScatterFusion*>(emitter.get());
  ASSERT_NE(fusion, nullptr);

  constexpr auto kUpdatesIndexing = R"(
    (th_x, th_y, th_z, bl_x, bl_y, bl_z)[chunk_id, unroll_id] -> (
      (bl_x * 128 + th_x) floordiv 200,
      ((bl_x * 128 + th_x) floordiv 20) mod 10,
      (bl_x * 128 + th_x) mod 20
    ),
    domain:
    th_x in [0, 127],
    th_y in [0, 0],
    th_z in [0, 0],
    bl_x in [0, 65],
    bl_y in [0, 0],
    bl_z in [0, 0],
    chunk_id in [0, 0],
    unroll_id in [0, 0],
    bl_x * 128 + th_x in [0, 8399]
  )";
  mlir::SmallVector<std::string> dim_names = {"th_x", "th_y", "th_z",
                                              "bl_x", "bl_y", "bl_z"};
  mlir::SmallVector<std::string> range_names = {"chunk_id", "unroll_id"};
  EXPECT_THAT(
      ToString(*fusion->ComputeThreadIdToInputIndexing(
                   /*root_index=*/0, /*hero_operand_index=*/3, &mlir_context_),
               dim_names, range_names, {}),
      MatchIndexingString(kUpdatesIndexing));
  EXPECT_THAT(
      ToString(*fusion->ComputeThreadIdToInputIndexing(
                   /*root_index=*/0, /*hero_operand_index=*/4, &mlir_context_),
               dim_names, range_names, {}),
      MatchIndexingString(kUpdatesIndexing));
  EXPECT_THAT(
      ToString(*fusion->ComputeThreadIdToInputIndexing(
                   /*root_index=*/1, /*hero_operand_index=*/3, &mlir_context_),
               dim_names, range_names, {}),
      MatchIndexingString(kUpdatesIndexing));
  EXPECT_THAT(
      ToString(*fusion->ComputeThreadIdToInputIndexing(
                   /*root_index=*/1, /*hero_operand_index=*/4, &mlir_context_),
               dim_names, range_names, {}),
      MatchIndexingString(kUpdatesIndexing));

  range_names.push_back("index_id");
  constexpr auto kIndicesIndexing = R"(
    (th_x, th_y, th_z, bl_x, bl_y, bl_z)[chunk_id, unroll_id, index_id] ->
      ((bl_x * 128 + th_x) floordiv 200, 0),
    domain:
    th_x in [0, 127],
    th_y in [0, 0],
    th_z in [0, 0],
    bl_x in [0, 65],
    bl_y in [0, 0],
    bl_z in [0, 0],
    chunk_id in [0, 0],
    unroll_id in [0, 0],
    index_id in [0, 0],
    bl_x * 128 + th_x in [0, 8399]
  )";
  EXPECT_THAT(
      ToString(*fusion->ComputeThreadIdToInputIndexing(
                   /*root_index=*/0, /*hero_operand_index=*/2, &mlir_context_),
               dim_names, range_names, {}),
      MatchIndexingString(kIndicesIndexing));
  EXPECT_THAT(
      ToString(*fusion->ComputeThreadIdToInputIndexing(
                   /*root_index=*/1, /*hero_operand_index=*/2, &mlir_context_),
               dim_names, range_names, {}),
      MatchIndexingString(kIndicesIndexing));
}

}  // namespace
}  // namespace gpu
}  // namespace xla
