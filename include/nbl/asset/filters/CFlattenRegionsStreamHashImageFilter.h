// Copyright (C) 2018-2024 - DevSH Graphics Programming Sp. z O.O.
// This file is part of the "Nabla Engine".
// For conditions of distribution and use, see copyright notice in nabla.h

#ifndef __NBL_ASSET_C_FLATTEN_REGIONS_STREAM_HASH_IMAGE_FILTER_H_INCLUDED__
#define __NBL_ASSET_C_FLATTEN_REGIONS_STREAM_HASH_IMAGE_FILTER_H_INCLUDED__

#include <type_traits>

#include "nbl/core/declarations.h"

#include "nbl/asset/filters/CMatchedSizeInOutImageFilterCommon.h"
#include "nbl/asset/filters/CFlattenRegionsImageFilter.h"
#include "blake/c/blake3.h"

namespace nbl
{
namespace asset
{
class CFlattenRegionsStreamHashImageFilter : public CImageFilter<CFlattenRegionsStreamHashImageFilter>, public CMatchedSizeInOutImageFilterCommon
{
	public:
		virtual ~CFlattenRegionsStreamHashImageFilter() {}

		struct ScratchMemory
		{
			core::smart_refctd_ptr<asset::ICPUImage> flatten; // for flattening input regions & prefilling with 0-value texels not covered by regions 
			core::smart_refctd_ptr<asset::ICPUBuffer> heap; // for storing hashes, single hash is obtained from given miplevel & layer, full hash for an image is a hash of this stack
		};
		
		class CState : public IImageFilter::IState
		{
			public:
				virtual ~CState() {}

				using hash_t = std::array<size_t, 4u>;

				const ICPUImage*					inImage = nullptr;
				hash_t								outHash = { {0} };
				ScratchMemory						scratchMemory = { .flatten = nullptr };
		};
		using state_type = CState;

		static inline ScratchMemory allocateScratchMemory(const asset::ICPUImage* inImage)
		{
			ScratchMemory scratch;
			scratch.flatten = inImage->clone();

			const auto& parameters = scratch.flatten->getCreationParameters();
			scratch.heap = core::make_smart_refctd_ptr<asset::ICPUBuffer>(parameters.mipLevels * parameters.arrayLayers * sizeof(CState::outHash));

			return scratch;
		}

		static inline bool validate(state_type* state)
		{
			if (!state)
				return false;

			if(!state->inImage)
				return false;

			if(!state->scratchMemory.flatten)
				return false;

			if (state->scratchMemory.flatten->getBuffer()->getSize() != state->inImage->getBuffer()->getSize())
				return false;

			if (!state->scratchMemory.heap)
				return false;

			const auto& parameters = state->inImage->getCreationParameters();

			if (state->scratchMemory.heap->getSize() != parameters.mipLevels * parameters.arrayLayers * sizeof(CState::outHash))
				return false;

			CFlattenRegionsImageFilter::state_type flatten;
			{
				flatten.inImage = state->inImage;
				flatten.outImage = state->scratchMemory.flatten;

				if (!CFlattenRegionsImageFilter::validate(&flatten)) // just to not DRY some of extra common validation steps
					return false;
			}

			return true;
		}

		template<class ExecutionPolicy>
		static inline bool execute(ExecutionPolicy&& policy, state_type* state)
		{
			if (!validate(state))
				return false;

			struct
			{
				CFlattenRegionsImageFilter::state_type flatten;			// CFlattenRegionsImageFilter's state, we use scratch memory to respecify regions & prefill with 0-value non-covered texels
			} proxy;

			/*
				first we need to ensure that

				- input image is flatten so there are no overlapping regions
				- texels which are not covered by regions are filled with 0

				NOTE: I'm aware of heavy image copy (TODO: I will try to do it without the copy) but flatten is handy to fill texels which are not covered by regions
			*/

			{
				auto& flatten = proxy.flatten;
				flatten.inImage = state->inImage;
				flatten.outImage = state->scratchMemory.flatten;
				flatten.preFill = true;
				memset(proxy.fillValue.pointer, 0, sizeof(flatten.fillValue.pointer));

				assert(CFlattenRegionsImageFilter::execute(&proxy.flatten)); // this should never fail, at this point we already are validated
			}

			/*
				now when the output is prepared we ignore respecified regions and go
				with single region covering all texels for a given mip level & layer 
			*/

			const auto& parameters = proxy.flatten.outImage->getCreationParameters();
			const uint8_t* inData = reinterpret_cast<const uint8_t*>(proxy.flatten.outImage->getBuffer()->getPointer());
			const TexelBlockInfo info(parameters.format);
			const core::rational<size_t> bytesPerPixel = proxy.flatten.outImage->getBytesPerPixel();
			const auto texelOrBlockByteSize = asset::getBlockDimensions(parameters.format);

			// override regions, we need to cover all texels
			auto regions = core::make_refctd_dynamic_array<core::smart_refctd_dynamic_array<IImage::SBufferCopy>>(parameters.mipLevels);
			{
				size_t bufferSize = 0ull;

				for (auto rit = regions->begin(); rit != regions->end(); rit++)
				{
					auto mipLevel = static_cast<uint32_t>(std::distance(regions->begin(), rit));
					auto localExtent = proxy.flatten.outImage->getMipSize(miplevel);
					rit->bufferOffset = bufferSize;
					rit->bufferRowLength = localExtent.x; // could round up to multiple of 8 bytes in the future
					rit->bufferImageHeight = localExtent.y;
					rit->imageSubresource.aspectMask = static_cast<IImage::E_ASPECT_FLAGS>(0u);
					rit->imageSubresource.mipLevel = mipLevel;
					rit->imageSubresource.baseArrayLayer = 0u;
					rit->imageSubresource.layerCount = inParams.arrayLayers;
					rit->imageOffset = { 0u,0u,0u };
					rit->imageExtent = { localExtent.x,localExtent.y,localExtent.z };
					auto levelSize = info.roundToBlockSize(localExtent);
					auto memsize = size_t(levelSize[0] * levelSize[1]) * size_t(levelSize[2] * parameters.arrayLayers) * bytesPerPixel;

					assert(memsize.getNumerator() % memsize.getDenominator() == 0u);
					bufferSize += memsize.getIntegerApprox();
				}
			}

			auto executePerMipLevel = [maxMipLevels = parameters.mipLevels](const uint32_t miplevel)
			{
				/*
					we stream-hash texels per given mip level & layer
				*/

				blake3_hasher hashers[parameters.arrayLayers];

				for (auto layer = 0u; layer < parameters.arrayLayers; ++layer)
					blake3_hasher_init(&hashers[layer]);

				auto hash = [&hashers, &miplevel, &parameters](uint32_t readBlockArrayOffset, core::vectorSIMDu32 readBlockPos) -> void
				{
					auto* hasher = hashers + readBlockPos.w;
					blake3_hasher_update(hasher, inData + readBlockArrayOffset, texelOrBlockByteSize);
				};

				IImage::SSubresourceLayers subresource = { .aspectMask = static_cast<IImage::E_ASPECT_FLAGS>(0u), .mipLevel = miplevel, .baseArrayLayer = 0u, .layerCount = parameters.arrayLayers }; // stick to given mip level and take all layers
				CMatchedSizeInOutImageFilterCommon::state_type::TexelRange range = { .offset = {}, .extent = { parameters.extent.width, parameters.extent.height, parameters.extent.depth } }; // cover all texels, take 0th mip level size to not clip anything at all
				CBasicImageFilterCommon::clip_region_functor_t clipFunctor(subresource, range, parameters.format);

				CBasicImageFilterCommon::executePerRegion(policy, proxy.flatten.outImage.get(), hash, regions.begin(), regions.end(), clipFunctor); // fire the hasher for layers with specified execution policy

				for (auto layer = 0u; layer < parameters.arrayLayers; ++layer)
				{
					auto* out = reinterpret_cast<CState::hash_t*>(state->scratchMemory.heap) + (miplevel * maxMipLevels)) + layer;
					blake3_hasher_finalize(hashers + layer, out, sizeof(CState::hash_t)); // finalize hash for layer + put it to heap for given mip level
				}
			};

			for (auto miplevel = 0u; miplevel < parameters.mipLevels; ++miplevel)
				executePerMipLevel(miplevel); // this can be easily used with par policy

			/*
				scratch's heap is filled with all hashes, time to use them and compute final hash
			*/

			blake3_hasher hasher;
			blake3_hasher_init(&hasher);
			{
				for (auto miplevel = 0u; miplevel < parameters.mipLevels; ++miplevel)
					for (auto layer = 0u; layer < parameters.arrayLayers; ++layer)
					{
						auto* hash = reinterpret_cast<CState::hash_t*>(state->scratchMemory.heap) + miplevel * parameters.mipLevels + layer;
						blake3_hasher_update(&hasher, hash, sizeof(CState::hash_t));
					}

				blake3_hasher_finalize(&hasher, state->outHash, sizeof(CState::hash_t)); // finalize output hash for whole image given all hashes
			}

			return true;
		}
		static inline bool execute(state_type* state)
		{
			return execute(core::execution::seq, state);
		}
};

} // end namespace asset
} // end namespace nbl

#endif // __NBL_ASSET_C_FLATTEN_REGIONS_STREAM_HASH_IMAGE_FILTER_H_INCLUDED__