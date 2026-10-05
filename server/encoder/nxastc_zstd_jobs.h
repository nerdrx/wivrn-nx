#pragma once

#include <initializer_list>
#include <utility>
#include <zstd.h>

namespace wivrn::nxastc_zstd_jobs
{
inline constexpr int workers = 2;
inline constexpr int job_size = 512 * 1024;
inline constexpr int overlap_log = 1;

inline size_t configure(ZSTD_CCtx * context)
{
	for (const auto & [parameter, value]: {
	             std::pair{ZSTD_c_compressionLevel, 3},
	             std::pair{ZSTD_c_nbWorkers, workers},
	             std::pair{ZSTD_c_jobSize, job_size},
	             std::pair{ZSTD_c_overlapLog, overlap_log},
	     })
	{
		const size_t result = ZSTD_CCtx_setParameter(context, parameter, value);
		if (ZSTD_isError(result))
			return result;
	}
	return 0;
}

inline size_t compress(ZSTD_CCtx * context,
                       void * destination,
                       size_t destination_capacity,
                       const void * source,
                       size_t source_size,
                       bool enabled,
                       int level)
{
	return enabled ? ZSTD_compress2(context, destination, destination_capacity, source, source_size)
	               : ZSTD_compressCCtx(context, destination, destination_capacity, source, source_size, level);
}
} // namespace wivrn::nxastc_zstd_jobs
