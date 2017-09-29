/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (c) 2017 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *******************************************************************************/

#include "miopen/solver.hpp"
#include "miopen/mlo_utils.hpp"

namespace miopen {
namespace solver {

bool ConvOclBwdWrW2::IsApplicable(const ConvolutionContext& params) const
{
    return ((params.kernel_size0 >= params.kernel_size1) &&
            ((params.kernel_stride0 > 1 || params.kernel_stride1 > 1) ||
             (params.kernel_size0 > 5) || (params.kernel_size0 == 5 && params.in_width >= 64))) ||
           ((params.pad0 == 0 || params.pad1 == 0) &&
            (params.kernel_size0 != 1 || params.kernel_size1 != 1));
}

ConvSolution ConvOclBwdWrW2::GetSolution(const ConvolutionContext& params,
                                         const PerformanceConfig&) const
{
    static const char* s_stride_table[32][2] = {
        //
        {"32x38x166x5x10x0x0x2x2x32x79x341x4", "1.2.2.4.12.2"},
        {"32x38x166x5x10x0x0x2x2x32x79x341x8", "1.2.2.4.19.2"},
        {"32x38x166x5x10x0x0x2x2x32x79x341x16", "1.2.2.4.19.2"},
        {"32x38x166x5x10x0x0x2x2x32x79x341x32", "1.2.2.4.19.2"},
        {"32x79x341x5x20x0x0x2x2x1x161x700x4", "1.1.4.2.12.2"},
        {"32x79x341x5x20x0x0x2x2x1x161x700x8", "1.1.4.2.12.2"},
        {"32x79x341x5x20x0x0x2x2x1x161x700x16", "1.1.4.2.12.2"},
        {"32x79x341x5x20x0x0x2x2x1x161x700x32", "1.2.4.2.12.2"},
        {"96x55x55x11x11x0x0x4x4x3x227x227x50", "1.2.2.4.11.5"},
        {"96x55x55x11x11x0x0x4x4x3x227x227x128", "1.2.2.4.11.5"},
        {"96x55x55x11x11x0x0x4x4x3x227x227x256", "1.2.2.4.11.5"},
        {"64x55x55x11x11x2x2x4x4x3x224x224x128", "1.2.2.4.11.5"},
        {"64x112x112x7x7x3x3x2x2x3x224x224x16", "1.2.4.4.8.7"},
        {"64x54x54x3x3x1x1x2x2x3x108x108x8", "1.4.4.4.9.9"},
        {nullptr, nullptr},
    };

    std::string key;

    key = std::to_string(params.n_inputs) + "x" + std::to_string(params.in_height) + "x" +
          std::to_string(params.in_width) + "x" + std::to_string(params.kernel_size1) + "x" +
          std::to_string(params.kernel_size0) + "x" + std::to_string(params.pad1) + "x" +
          std::to_string(params.pad0) + "x" + std::to_string(params.kernel_stride1) + "x" +
          std::to_string(params.kernel_stride0) + "x" + std::to_string(params.n_outputs) + "x" +
          std::to_string(params.out_height) + "x" + std::to_string(params.out_width) + "x" +
          std::to_string(params.batch_sz);
    //	std::map<std::string, std::string> lcl_db;
    bool found = false;
    int i      = 0;
    for(; s_stride_table[i][0] != nullptr; ++i)
    {
        if(std::string(s_stride_table[i][0]) == key)
        {
            found = true;
            break;
        }
    }

    ConvSolution result;

    int N_BATCH_LOOPS = 1; // _batch_sz / _n_stacks;
                           // n of map in a block (see below)
    result.out_pix_tile1 = (params.out_width > 512) ? 1 : 2;
    int n_waves          = (params.kernel_size0 > 11) ? 2 : 4;
    // n of shared blocks of output maps in lcl memory
    result.n_out_pix_tiles = 2;
    int read_unit          = 6;

    int N_ALIGNED_OUT_SCAN_BLK = (params.out_width > 512) ? 1 : 2;

    if(found)
    {
        std::vector<std::string> val_vec;

        tokenize(std::string(s_stride_table[i][1]), val_vec, std::string("."));

        N_BATCH_LOOPS          = std::stoi(val_vec[0]);
        result.out_pix_tile1   = std::stoi(val_vec[1]);
        n_waves                = std::stoi(val_vec[2]);
        result.n_out_pix_tiles = std::stoi(val_vec[3]);
        read_unit              = std::stoi(val_vec[4]);
        N_ALIGNED_OUT_SCAN_BLK = std::stoi(val_vec[5]);
    }

    // size_t localMemSize = 64 * 1024;

    const auto _hw_wave_sz = 64;
    //_dev_local_mem_sz = localMemSize; // in bytes

    // number  of batch iterations
    result.n_stacks = 1;
    result.n_stacks = std::min(params.batch_sz, result.n_stacks);
    int n_batch_blks =
        (params.batch_sz + N_BATCH_LOOPS * result.n_stacks - 1) / (N_BATCH_LOOPS * result.n_stacks);

    if(params.n_passes)
    {
        result.passes = (n_batch_blks > 1) ? 2 : 1;
        return result;
    }

    // number of filter taps in the processing wk_item
    int WEI_WKITEM =
        (params.kernel_size0 <= 7 || (((params.kernel_size0 / 2) * 2) != params.kernel_size0))
            ? params.kernel_size0
            : params.kernel_size0 / 2;

    result.in_tile0      = 1;
    result.in_tile1      = 1;
    result.out_pix_tile0 = 1;

    result.n_in_data_tiles = 1;

    // select output mapping
    int total_out_maps   = result.n_out_pix_tiles * result.out_pix_tile1;
    result.out_pix_tile1 = (total_out_maps > params.n_inputs) ? 1 : result.out_pix_tile1;
    total_out_maps       = result.n_out_pix_tiles * result.out_pix_tile1;
    result.n_out_pix_tiles =
        (total_out_maps > params.n_inputs) ? params.n_inputs : result.n_out_pix_tiles;
    int N_OUT_BLK_GRP = result.out_pix_tile1;
    total_out_maps    = result.n_out_pix_tiles * result.out_pix_tile1;

    // each wave is a filter row
    int GRP_SZ = _hw_wave_sz * n_waves;

    // inpout are outputs
    int wei_cstride = params.kernel_size0 * params.kernel_size1;
    int wei_bstride = params.n_outputs * wei_cstride;

    //	int read_unit = 6;
    std::string READ_TYPE = (read_unit == 1) ? "_FLOAT" : "_FLOAT" + std::to_string((read_unit));

    // input is output
    int ALIGNED_OUT_SCAN_LN = ((params.in_width + read_unit - 1) / read_unit); // image aligned scan

    int N_OUT_BLK = (params.in_height + N_ALIGNED_OUT_SCAN_BLK - 1) / N_ALIGNED_OUT_SCAN_BLK;

    int lcl_mem_sz = N_ALIGNED_OUT_SCAN_BLK * ALIGNED_OUT_SCAN_LN * read_unit +
                     params.out_width * ((N_ALIGNED_OUT_SCAN_BLK - 1) * params.kernel_stride1 +
                                         params.kernel_size1);
    if(lcl_mem_sz > 8 * 1024)
    {
        return ConvSolution(miopenStatusNotInitialized);
    }

    int OUT_N_PIXS_OFF = params.in_width - (params.in_width / read_unit) * read_unit;

    result.grp_tile0 = GRP_SZ;
    result.grp_tile1 = 1;
    int grp_tile2    = 1;

    // utility parameters
    int n_ut_waves = 4;
    int UT_GRP_SZ0 = _hw_wave_sz * n_ut_waves;
    int ut_read_unit =
        ((wei_cstride / 4) * 4 == wei_cstride) ? 4 : ((wei_cstride / 2) * 2 == wei_cstride) ? 2 : 1;
    std::string UT_READ_TYPE =
        (ut_read_unit == 1) ? "_FLOAT" : "_FLOAT" + std::to_string((ut_read_unit));

    // it's backward - inputs are outputs and vs versa
    const auto comp_options =
        std::string(" -DMLO_DIR_FORWARD=") + std::to_string((params.forward)) +
        std::string(" -DMLO_GRP_SZ=") + std::to_string((GRP_SZ)) + std::string(" -DMLO_GRP_SZ0=") +
        std::to_string((result.grp_tile0)) + std::string(" -DMLO_GRP_SZ1=") +
        std::to_string((result.grp_tile1)) + std::string(" -DMLO_GRP_SZ2=") +
        std::to_string((grp_tile2)) + std::string(" -DMLO_FILTER_SIZE0=") +
        std::to_string(params.kernel_size0) + std::string(" -DMLO_FILTER_SIZE1=") +
        std::to_string(params.kernel_size1) + std::string(" -DMLO_FILTER_PAD0=") +
        std::to_string(params.pad0) + std::string(" -DMLO_FILTER_PAD1=") +
        std::to_string(params.pad1) + std::string(" -DMLO_FILTER_STRIDE0=") +
        std::to_string(params.kernel_stride0) + std::string(" -DMLO_FILTER_STRIDE1=") +
        std::to_string(params.kernel_stride1) + std::string(" -DMLO_N_OUTPUTS=") +
        std::to_string(params.n_inputs) + std::string(" -DMLO_N_INPUTS=") +
        std::to_string(params.n_outputs) + std::string(" -DMLO_BATCH_SZ=") +
        std::to_string(params.batch_sz) + std::string(" -DMLO_N_BATCH_LOOPS=") +
        std::to_string(N_BATCH_LOOPS) + std::string(" -DMLO_OUT_BATCH_STRIDE=") +
        std::to_string((params.in_batch_stride)) + std::string(" -DMLO_OUT_CHANNEL_STRIDE=") +
        std::to_string((params.in_channel_stride)) + std::string(" -DMLO_OUT_STRIDE=") +
        std::to_string((params.in_stride)) + std::string(" -DMLO_IN_BATCH_STRIDE=") +
        std::to_string((params.out_batch_stride)) + std::string(" -DMLO_IN_CHANNEL_STRIDE=") +
        std::to_string((params.out_channel_stride)) + std::string(" -DMLO_IN_STRIDE=") +
        std::to_string((params.out_stride)) + std::string(" -DMLO_WEI_BATCH_STRIDE=") +
        std::to_string((wei_bstride)) + std::string(" -DMLO_WEI_CHANNEL_STRIDE=") +
        std::to_string((wei_cstride)) + std::string(" -DMLO_IN_WIDTH=") +
        std::to_string((params.out_width)) + std::string(" -DMLO_IN_HEIGHT=") +
        std::to_string(params.out_height) + std::string(" -DMLO_OUT_WIDTH=") +
        std::to_string(params.in_width) + std::string(" -DMLO_OUT_HEIGHT=") +
        std::to_string(params.in_height) + std::string(" -DMLO_IN_TILE1=") +
        std::to_string(result.in_tile1) + std::string(" -DMLO_IN_TILE0=") +
        std::to_string(result.in_tile0) + std::string(" -DMLO_N_LCL_BATCHS=") +
        std::to_string(result.n_stacks) // # of diff stacks (part of batch).
        + std::string(" -DMLO_N_LCL_OUT_MAPS=") +
        std::to_string(result.n_out_pix_tiles) // # output pixel tiles per wk-item (ALU)
        + std::string(" -DMLO_N_LCL_IN_MAPS=") +
        std::to_string(result.n_in_data_tiles) // total # of blocks of different inputs in LDS
        + std::string(" -DMLO_OUT_TILE0=") +
        std::to_string((result.out_pix_tile0)) // size of ouptput tile per wk-item (ALU))
        + std::string(" -DMLO_OUT_TILE1=") + std::to_string(result.out_pix_tile1) //
        + std::string(" -DMLO_N_WAVES=") + std::to_string(n_waves) +
        std::string(" -DMLO_READ_TYPE=") + READ_TYPE + std::string(" -DMLO_READ_UNIT=") +
        std::to_string(read_unit) + std::string(" -DMLO_ALIGNED_OUT_SCAN_LN=") +
        std::to_string(ALIGNED_OUT_SCAN_LN) // image aligned scan
        + std::string(" -DMLO_N_ALIGNED_OUT_SCAN_BLK=") + std::to_string(N_ALIGNED_OUT_SCAN_BLK) +
        std::string(" -DMLO_WEI_WKITEM=") + std::to_string(WEI_WKITEM) +
        std::string(" -DMLO_N_OUT_BLK_GRP=") + std::to_string(N_OUT_BLK_GRP) +
        std::string(" -DMLO_N_OUT_BLK=") + std::to_string(N_OUT_BLK) +
        std::string(" -DMLO_HW_WAVE_SZ=") + std::to_string(_hw_wave_sz) +
        std::string(" -DMLO_LG2_PHYS_WAVE_SZ=") + std::to_string(mloLg2(_hw_wave_sz)) +
        std::string(" -DMLO_OUT_N_PIXS_OFF=") + std::to_string(OUT_N_PIXS_OFF)

        + std::string(" -DMLO_CONV_BIAS=") + std::to_string(params.bias)

        + std::string(" -DMLO_UT_READ_TYPE=") + UT_READ_TYPE + std::string(" -DMLO_UT_READ_UNIT=") +
        std::to_string(ut_read_unit)

        + std::string(" -DMLO_UT_GRP_SZ0=") + std::to_string((UT_GRP_SZ0))

        //		+ std::string(" -limit-vector-registers=64 ")
        + params.general_compile_options;

    // wrt to W
    {
        KernelInfo kernel;

        kernel.l_wk.push_back(result.grp_tile0);
        kernel.l_wk.push_back(result.grp_tile1);
        kernel.l_wk.push_back(grp_tile2);
        // input is output

        size_t gbl_wk0 = GRP_SZ * params.n_outputs;
        size_t gbl_wk1 = ((params.n_inputs + total_out_maps - 1) / total_out_maps);
        size_t gbl_wk2 = n_batch_blks;

        kernel.g_wk.push_back(gbl_wk0);
        kernel.g_wk.push_back(gbl_wk1);
        kernel.g_wk.push_back(gbl_wk2);

        kernel.kernel_file  = "MIOpenConvBwdWrWS2.cl";
        kernel.kernel_name  = "MIOpenCvBwdWrW";
        kernel.comp_options = comp_options;

        result.construction_params.push_back(kernel);
        result.workspce_sz = 0;
    }

    // sum over batch
    if(n_batch_blks > 1)
    {
        KernelInfo kernel;

        kernel.kernel_file  = "MIOpenConvBwdWrWS2.cl";
        kernel.kernel_name  = "MIOpenCvBwdWrW_rdc";
        kernel.comp_options = comp_options;

        kernel.l_wk.push_back(UT_GRP_SZ0);
        kernel.l_wk.push_back(1);
        kernel.l_wk.push_back(1);

        int gbl_ut_wk0 = wei_bstride * params.n_inputs / ut_read_unit;

        kernel.g_wk.push_back(gbl_ut_wk0);
        kernel.g_wk.push_back(1);
        kernel.g_wk.push_back(1);
        result.construction_params.push_back(kernel);

        int data_len       = (params.out_data_type == "FP32" ? 4 : 8);
        result.workspce_sz = wei_bstride * params.n_inputs * n_batch_blks * data_len;
    }
    return result;
}
} // namespace solver
} // namespace miopen
