#include <iostream>
#include <map>
#include <ranges>
#include <vector>
#include <utility>

#include "nbl/system/IApplicationFramework.h"
#include "nbl/system/CStdoutLogger.h"
#include "nbl/ext/ImGui/ImGui.h"
#include "shaders/common.hlsl"
#include "nbl/ext/ImGui/builtin/builtinResources.h"
#include "nbl/ext/ImGui/builtin/CArchive.h"
#include "imgui/imgui.h"
#include "imgui/misc/cpp/imgui_stdlib.h"

using namespace nbl::video;
using namespace nbl::core;
using namespace nbl::asset;
using namespace nbl::ui;

namespace nbl::ext::imgui
{
	static constexpr auto MDI_COMPONENT_COUNT = UI::MDI::EBC_COUNT;
	static constexpr auto MDI_ALIGNMENTS = std::to_array<typename UI::MDI::ALLOCATOR_TRAITS_T::size_type>({ alignof(VkDrawIndexedIndirectCommand), alignof(PerObjectData), alignof(ImDrawIdx), alignof(ImDrawVert) });
	static constexpr auto MDI_MAX_ALIGNMENT = *std::max_element(MDI_ALIGNMENTS.begin(), MDI_ALIGNMENTS.end());

	void UI::createPipeline()
	{
		SPushConstantRange pushConstantRanges[] = 
		{
			{
				.stageFlags = IShader::E_SHADER_STAGE::ESS_VERTEX | IShader::E_SHADER_STAGE::ESS_FRAGMENT,
				.offset = 0,
				.size = sizeof(PushConstants)
			}
		};

		auto pipelineLayout = m_creationParams.resources.pipelineLayout ? smart_refctd_ptr<IGPUPipelineLayout>(m_creationParams.resources.pipelineLayout) /* provided? good, at this point its validated and we just use it */ :
		[&]() -> smart_refctd_ptr<IGPUPipelineLayout>
		{
			//! if default pipeline layout is not provided, we create it here given your request binding info with certain constraints about samplers - they are immutable separate and part of the default layout
			smart_refctd_ptr<IGPUSampler> fontAtlasUISampler, userTexturesSampler;

			using binding_flags_t = IGPUDescriptorSetLayout::SBinding::E_CREATE_FLAGS;
			{
				IGPUSampler::SParams params;
				params.AnisotropicFilter = 1u;
				params.TextureWrapU = ISampler::ETC_REPEAT;
				params.TextureWrapV = ISampler::ETC_REPEAT;
				params.TextureWrapW = ISampler::ETC_REPEAT;

				fontAtlasUISampler = m_creationParams.utilities->getLogicalDevice()->createSampler(params);
				fontAtlasUISampler->setObjectDebugName("Nabla default ImGUI font UI sampler");
			}

			{
				IGPUSampler::SParams params;
				params.MinLod = 0.f;
				params.MaxLod = 0.f;
				params.TextureWrapU = ISampler::ETC_CLAMP_TO_EDGE;
				params.TextureWrapV = ISampler::ETC_CLAMP_TO_EDGE;
				params.TextureWrapW = ISampler::ETC_CLAMP_TO_EDGE;

				userTexturesSampler = m_creationParams.utilities->getLogicalDevice()->createSampler(params);
				userTexturesSampler->setObjectDebugName("Nabla default ImGUI custom texture sampler");
			}

			//! as mentioned we use immutable separate samplers and they are part of the descriptor set layout
			std::vector<core::smart_refctd_ptr<IGPUSampler>> immutableSamplers(m_creationParams.resources.count);
			for (auto& it : immutableSamplers)
				it = smart_refctd_ptr(userTexturesSampler);

			immutableSamplers[nbl::ext::imgui::UI::NBL_FONT_ATLAS_TEX_ID] = smart_refctd_ptr(fontAtlasUISampler);

			auto textureBinding = IGPUDescriptorSetLayout::SBinding
			{
				.binding = m_creationParams.resources.textures.bindingIx,
				.type = IDescriptor::E_TYPE::ET_SAMPLED_IMAGE,
				.createFlags = m_creationParams.resources.TEXTURES_REQUIRED_CREATE_FLAGS,
				.stageFlags = m_creationParams.resources.RESOURCES_REQUIRED_STAGE_FLAGS,
				.count = m_creationParams.resources.count
			};

			auto samplersBinding = IGPUDescriptorSetLayout::SBinding
			{
				.binding = m_creationParams.resources.samplers.bindingIx,
				.type = IDescriptor::E_TYPE::ET_SAMPLER,
				.createFlags = m_creationParams.resources.SAMPLERS_REQUIRED_CREATE_FLAGS,
				.stageFlags = m_creationParams.resources.RESOURCES_REQUIRED_STAGE_FLAGS,
				.count = m_creationParams.resources.count,
				.immutableSamplers = immutableSamplers.data()
			};

			auto layouts = std::to_array<smart_refctd_ptr<IGPUDescriptorSetLayout>>({nullptr, nullptr, nullptr, nullptr });

			if (m_creationParams.resources.textures.setIx == m_creationParams.resources.samplers.setIx)
				layouts[m_creationParams.resources.textures.setIx] = m_creationParams.utilities->getLogicalDevice()->createDescriptorSetLayout({{textureBinding, samplersBinding}});
			else
			{
				layouts[m_creationParams.resources.textures.setIx] = m_creationParams.utilities->getLogicalDevice()->createDescriptorSetLayout({{textureBinding}});
				layouts[m_creationParams.resources.samplers.setIx] = m_creationParams.utilities->getLogicalDevice()->createDescriptorSetLayout({{samplersBinding}});
			}

			assert(layouts[m_creationParams.resources.textures.setIx]);
			assert(layouts[m_creationParams.resources.samplers.setIx]);

			return m_creationParams.utilities->getLogicalDevice()->createPipelineLayout(pushConstantRanges, std::move(layouts[0u]), std::move(layouts[1u]), std::move(layouts[2u]), std::move(layouts[3u]));
		}();

		if (!pipelineLayout)
		{
			m_creationParams.utilities->getLogger()->log("Could not create pipeline layout!", system::ILogger::ELL_ERROR);
			assert(false);
		}

		struct
		{
			core::smart_refctd_ptr<video::IGPUShader> vertex, fragment;
		} shaders;

		{
			constexpr std::string_view NBL_ARCHIVE_ALIAS = "nbl/ext/imgui/shaders";
				
			auto system = smart_refctd_ptr<system::ISystem>(m_creationParams.assetManager->getSystem());																	//! proxy the system, we will touch it gently
			auto archive = make_smart_refctd_ptr<nbl::ext::imgui::builtin::CArchive>(smart_refctd_ptr<system::ILogger>(m_creationParams.utilities->getLogger()));		//! we should never assume user will mount our internal archive since its the extension and not user's job to do it, hence we mount only to compile our extension sources then unmount the archive
			auto compiler = make_smart_refctd_ptr<CHLSLCompiler>(smart_refctd_ptr(system));																				//! note we are out of default logical device's compiler set scope so also a few special steps are required to compile our extension shaders to SPIRV
			auto includeFinder = make_smart_refctd_ptr<IShaderCompiler::CIncludeFinder>(smart_refctd_ptr(system));
			auto includeLoader = includeFinder->getDefaultFileSystemLoader();
			includeFinder->addSearchPath(NBL_ARCHIVE_ALIAS.data(), includeLoader);

			auto createShader = [&]<core::StringLiteral key, asset::IShader::E_SHADER_STAGE stage>() -> core::smart_refctd_ptr<video::IGPUShader>
			{
		asset::IAssetLoader::SAssetLoadParams params = {};
				params.logger = m_creationParams.utilities->getLogger();
				params.workingDirectory = NBL_ARCHIVE_ALIAS.data();

				auto bundle = m_creationParams.assetManager->getAsset(key.value, params);
				const auto assets = bundle.getContents();

				if (assets.empty())
				{
					m_creationParams.utilities->getLogger()->log("Could not load \"%s\" shader!", system::ILogger::ELL_ERROR, key.value);
					return nullptr;
				}

				const auto shader = IAsset::castDown<asset::ICPUShader>(assets[0]);

				CHLSLCompiler::SOptions options = {};
				options.stage = stage;
				options.preprocessorOptions.sourceIdentifier = key.value;
				options.preprocessorOptions.logger = m_creationParams.utilities->getLogger();
				options.preprocessorOptions.includeFinder = includeFinder.get();

				auto compileToSPIRV = [&]() -> core::smart_refctd_ptr<ICPUShader>
				{
					#define NBL_DEFAULT_OPTIONS "-spirv", "-Zpr", "-enable-16bit-types", "-fvk-use-scalar-layout", "-Wno-c++11-extensions", "-Wno-c++1z-extensions", "-Wno-c++14-extensions", "-Wno-gnu-static-float-init", "-fspv-target-env=vulkan1.3", "-HV", "202x" /* default required params, just to not throw warnings */
					const std::string_view code (reinterpret_cast<const char*>(shader->getContent()->getPointer()), shader->getContent()->getSize());

					if constexpr (stage == IShader::E_SHADER_STAGE::ESS_VERTEX)
					{
						const auto VERTEX_COMPILE_OPTIONS = std::to_array<std::string>({NBL_DEFAULT_OPTIONS, "-T", "vs_6_7", "-E", "VSMain", "-O3"});
						options.dxcOptions = VERTEX_COMPILE_OPTIONS;

						return compiler->compileToSPIRV(code.data(), options); // we good here - no code patching
					}
					else if (stage == IShader::E_SHADER_STAGE::ESS_FRAGMENT)
					{
						const auto FRAGMENT_COMPILE_OPTIONS = std::to_array<std::string>({NBL_DEFAULT_OPTIONS, "-T", "ps_6_7", "-E", "PSMain", "-O3"});
						options.dxcOptions = FRAGMENT_COMPILE_OPTIONS;

						std::stringstream stream;

						stream << "// -> this code has been autogenerated with Nabla ImGUI extension\n"
							<< "#define NBL_TEXTURES_BINDING_IX " << m_creationParams.resources.textures.bindingIx << "\n"
							<< "#define NBL_SAMPLER_STATES_BINDING_IX " << m_creationParams.resources.samplers.bindingIx << "\n"
							<< "#define NBL_TEXTURES_SET_IX " << m_creationParams.resources.textures.setIx << "\n"
							<< "#define NBL_SAMPLER_STATES_SET_IX " << m_creationParams.resources.samplers.setIx << "\n"
							<< "#define NBL_RESOURCES_COUNT " << m_creationParams.resources.count << "\n"
							<< "// <-\n\n";

						const auto newCode = stream.str() + std::string(code);
						return compiler->compileToSPIRV(newCode.c_str(), options); // but here we do patch the code with additional define directives for which values are taken from the creation parameters
					}
					else
					{
						static_assert(stage != IShader::E_SHADER_STAGE::ESS_UNKNOWN, "Unknown shader stage!");
						return nullptr;
					}
				};

				auto spirv = compileToSPIRV();

				if (!spirv)
				{
					m_creationParams.utilities->getLogger()->log("Could not compile \"%s\" shader!", system::ILogger::ELL_ERROR, key.value);
					return nullptr;
				}

				auto gpu = m_creationParams.utilities->getLogicalDevice()->createShader(spirv.get());

				if (!gpu)
					m_creationParams.utilities->getLogger()->log("Could not create GPU shader for \"%s\"!", system::ILogger::ELL_ERROR, key.value);

				return gpu;
			};

			system->mount(smart_refctd_ptr(archive), NBL_ARCHIVE_ALIAS.data());
			shaders.vertex = createShader.template operator() < NBL_CORE_UNIQUE_STRING_LITERAL_TYPE("vertex.hlsl"), IShader::E_SHADER_STAGE::ESS_VERTEX > ();
			shaders.fragment = createShader.template operator() < NBL_CORE_UNIQUE_STRING_LITERAL_TYPE("fragment.hlsl"), IShader::E_SHADER_STAGE::ESS_FRAGMENT > ();
			system->unmount(archive.get(), NBL_ARCHIVE_ALIAS.data());

			assert(shaders.vertex);
			assert(shaders.fragment);
		}
	
		SVertexInputParams vertexInputParams{};
		{
			vertexInputParams.enabledBindingFlags = 0b1u;
			vertexInputParams.enabledAttribFlags = 0b111u;

			vertexInputParams.bindings[0].inputRate = asset::SVertexInputBindingParams::EVIR_PER_VERTEX;
			vertexInputParams.bindings[0].stride = sizeof(ImDrawVert);

			auto& position = vertexInputParams.attributes[0];
			position.format = EF_R32G32_SFLOAT;
			position.relativeOffset = offsetof(ImDrawVert, pos);
			position.binding = 0u;

			auto& uv = vertexInputParams.attributes[1];
			uv.format = EF_R32G32_SFLOAT;
			uv.relativeOffset = offsetof(ImDrawVert, uv);
			uv.binding = 0u;

			auto& color = vertexInputParams.attributes[2];
			color.format = EF_R8G8B8A8_UNORM;
			color.relativeOffset = offsetof(ImDrawVert, col);
			color.binding = 0u;
		}

		SBlendParams blendParams{};
		{
			blendParams.logicOp = ELO_NO_OP;

			auto& param = blendParams.blendParams[0];

			// color blending factors (for RGB)
			param.srcColorFactor = EBF_SRC_ALPHA;
			param.dstColorFactor = EBF_ONE_MINUS_SRC_ALPHA;
			param.colorBlendOp = EBO_ADD;

			// alpha blending factors (for A)
			param.srcAlphaFactor = EBF_ONE;
			param.dstAlphaFactor = EBF_ONE_MINUS_SRC_ALPHA;
			param.alphaBlendOp = EBO_ADD;

			// Write all components (R, G, B, A)
			param.colorWriteMask = (1u << 0u) | (1u << 1u) | (1u << 2u) | (1u << 3u);
		}
	
		SRasterizationParams rasterizationParams{};
		{
			rasterizationParams.faceCullingMode = EFCM_NONE;
			rasterizationParams.depthWriteEnable = false;
			rasterizationParams.depthBoundsTestEnable = false;
		}

		SPrimitiveAssemblyParams primitiveAssemblyParams{};
		{
			primitiveAssemblyParams.primitiveType = EPT_TRIANGLE_LIST;
		}

		{
			const IGPUShader::SSpecInfo specs[] =
			{
				{ .entryPoint = "VSMain", .shader = shaders.vertex.get() },
				{ .entryPoint = "PSMain", .shader = shaders.fragment.get() }
			};

			IGPUGraphicsPipeline::SCreationParams params[1];
			{
				auto& param = params[0u];
				param.layout = pipelineLayout.get();
				param.shaders = specs;
				param.renderpass = m_creationParams.renderpass;
				param.cached = { .vertexInput = vertexInputParams, .primitiveAssembly = primitiveAssemblyParams, .rasterization = rasterizationParams, .blend = blendParams, .subpassIx = m_creationParams.subpassIx };
			};
			
			if (!m_creationParams.utilities->getLogicalDevice()->createGraphicsPipelines(m_creationParams.pipelineCache, params, &pipeline))
			{
				m_creationParams.utilities->getLogger()->log("Could not create pipeline!", system::ILogger::ELL_ERROR);
				assert(false);
			}
		}
	}

	ISemaphore::future_t<IQueue::RESULT> UI::createFontAtlasTexture(video::IGPUCommandBuffer* cmdBuffer)
	{
		// Load Fonts
		// - If no fonts are loaded, dear imgui will use the default font. You can also load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
		// - AddFontFromFileTTF() will return the ImFont* so you can store it if you need to select the font among multiple.
		// - If the file cannot be loaded, the function will return NULL. Please handle those errors in your application (e.g. use an assertion, or display an error and quit).
		// - The fonts will be rasterized at a given size (w/ oversampling) and stored into a texture when calling ImFontAtlas::Build()/GetTexDataAsXXXX(), which ImGui_ImplXXXX_NewFrame below will call.
		// - Read 'docs/FONTS.md' for more instructions and details.
		// - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
		//io.Fonts->AddFontDefault();
		//io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf", 16.0f);
		//io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf", 15.0f);
		//io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf", 16.0f);
		//io.Fonts->AddFontFromFileTTF("../../misc/fonts/ProggyTiny.ttf", 10.0f);
		//ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf", 18.0f, NULL, io.Fonts->GetGlyphRangesJapanese());
		ImGuiIO& io = ImGui::GetIO();

		// TODO: don't `pixels` need to be freed somehow!? (Use a uniqueptr with custom deleter lambda)
		uint8_t* pixels = nullptr;
		int32_t width, height;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
		io.Fonts->SetTexID(NBL_FONT_ATLAS_TEX_ID);

		if (!pixels || width<=0 || height<=0)
			return IQueue::RESULT::OTHER_ERROR;

		const size_t componentsCount = 4, image_size = width * height * componentsCount * sizeof(uint8_t);
		
		_NBL_STATIC_INLINE_CONSTEXPR auto NBL_FORMAT_FONT = EF_R8G8B8A8_UNORM;
		const auto buffer = core::make_smart_refctd_ptr< asset::CCustomAllocatorCPUBuffer<core::null_allocator<uint8_t>, true> >(image_size, pixels, core::adopt_memory);
		
		IGPUImage::SCreationParams params;
		params.flags = static_cast<IImage::E_CREATE_FLAGS>(0u);
		params.type = IImage::ET_2D;
		params.format = NBL_FORMAT_FONT;
		params.extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1u };
		params.mipLevels = 1;
		params.arrayLayers = 1u;
		params.samples = IImage::ESCF_1_BIT;
		params.usage |= IGPUImage::EUF_TRANSFER_DST_BIT | IGPUImage::EUF_SAMPLED_BIT | IGPUImage::E_USAGE_FLAGS::EUF_TRANSFER_SRC_BIT; // do you really need the SRC bit?

		struct
		{
			smart_refctd_dynamic_array<ICPUImage::SBufferCopy> data = make_refctd_dynamic_array<smart_refctd_dynamic_array<ICPUImage::SBufferCopy>>(1ull);		
			SRange <ICPUImage::SBufferCopy> range = { data->begin(), data->end() };
			IImage::SSubresourceRange subresource = 
			{
				.aspectMask = IImage::EAF_COLOR_BIT,
				.baseMipLevel = 0u,
				.levelCount = 1u,
				.baseArrayLayer = 0u,
				.layerCount = 1u
			};
		} regions;
		{
			auto* region = regions.data->begin();
			region->bufferOffset = 0ull;
			region->bufferRowLength = params.extent.width;
			region->bufferImageHeight = 0u;
			region->imageSubresource = {};
			region->imageSubresource.aspectMask = IImage::EAF_COLOR_BIT;
			region->imageSubresource.layerCount = 1u;
			region->imageOffset = { 0u, 0u, 0u };
			region->imageExtent = { params.extent.width, params.extent.height, 1u };
		}

		auto image = m_creationParams.utilities->getLogicalDevice()->createImage(std::move(params));

		if (!image)
		{
			m_creationParams.utilities->getLogger()->log("Could not create font image!", system::ILogger::ELL_ERROR);
			return IQueue::RESULT::OTHER_ERROR;
		}
		image->setObjectDebugName("Nabla IMGUI extension Font Image");

		if (!m_creationParams.utilities->getLogicalDevice()->allocate(image->getMemoryReqs(), image.get()).isValid())
		{
			m_creationParams.utilities->getLogger()->log("Could not allocate memory for font image!", system::ILogger::ELL_ERROR);
			return IQueue::RESULT::OTHER_ERROR;
		}
		
		image->setObjectDebugName("Nabla IMGUI extension Font Atlas");

		SIntendedSubmitInfo sInfo;
		{
			IQueue::SSubmitInfo::SCommandBufferInfo cmdInfo = { cmdBuffer };

			auto scratchSemaphore = m_creationParams.utilities->getLogicalDevice()->createSemaphore(0);
			if (!scratchSemaphore)
			{
				m_creationParams.utilities->getLogger()->log("Could not create scratch semaphore", system::ILogger::ELL_ERROR);
				return IQueue::RESULT::OTHER_ERROR;
			}
			scratchSemaphore->setObjectDebugName("Nabla IMGUI extension Scratch Semaphore");

			sInfo.queue = m_creationParams.transfer;
			sInfo.waitSemaphores = {};
			sInfo.commandBuffers = { &cmdInfo, 1 };
			sInfo.scratchSemaphore =
			{
				.semaphore = scratchSemaphore.get(),
				.value = 0u,
				.stageMask = PIPELINE_STAGE_FLAGS::ALL_TRANSFER_BITS
			};
			
			// we have no explicit source stage and access to sync against, brand new clean image.
			const asset::SMemoryBarrier toTransferDep = {
				.dstStageMask = PIPELINE_STAGE_FLAGS::COPY_BIT,
				.dstAccessMask = ACCESS_FLAGS::TRANSFER_WRITE_BIT,
			};
			const auto transferLayout = IGPUImage::LAYOUT::TRANSFER_DST_OPTIMAL;
			// transition to TRANSFER_DST
			IGPUCommandBuffer::SImageMemoryBarrier<IGPUCommandBuffer::SOwnershipTransferBarrier> barriers[] = 
			{ 
				{
					.barrier = {.dep = toTransferDep},
					.image = image.get(),
					.subresourceRange = regions.subresource,
					.oldLayout = IGPUImage::LAYOUT::UNDEFINED, // wiping transition
					.newLayout = transferLayout
				} 
			};

			cmdBuffer->begin(IGPUCommandBuffer::USAGE::ONE_TIME_SUBMIT_BIT);
			cmdBuffer->pipelineBarrier(E_DEPENDENCY_FLAGS::EDF_NONE,{.imgBarriers=barriers});
			// We cannot use the `AutoSubmit` variant of the util because we need to add a pipeline barrier with a transition onto the command buffer after the upload.
			// old layout is UNDEFINED because we don't want a content preserving transition, we can just put ourselves in transfer right away
			if (!m_creationParams.utilities->updateImageViaStagingBuffer(sInfo,pixels,image->getCreationParameters().format,image.get(),transferLayout,regions.range))
			{
				m_creationParams.utilities->getLogger()->log("Could not upload font image contents", system::ILogger::ELL_ERROR);
				return IQueue::RESULT::OTHER_ERROR;
			}

			// we only need to sync with semaphore signal
			barriers[0].barrier.dep = toTransferDep.nextBarrier(sInfo.scratchSemaphore.stageMask,ACCESS_FLAGS::NONE);
			// transition to READ_ONLY_OPTIMAL ready for rendering with sampling
			barriers[0].oldLayout = barriers[0].newLayout;
			barriers[0].newLayout = IGPUImage::LAYOUT::READ_ONLY_OPTIMAL;
			cmdBuffer->pipelineBarrier(E_DEPENDENCY_FLAGS::EDF_NONE,{.imgBarriers=barriers});
			cmdBuffer->end();

			const auto submit = sInfo.popSubmit({});
			if (m_creationParams.transfer->submit(submit)!=IQueue::RESULT::SUCCESS)
			{
				m_creationParams.utilities->getLogger()->log("Could not submit workload for font texture upload.", system::ILogger::ELL_ERROR);
				return IQueue::RESULT::OTHER_ERROR;
			}
		}
		 
		{
			IGPUImageView::SCreationParams params;
			params.format = image->getCreationParameters().format;
			params.viewType = IImageView<IGPUImage>::ET_2D;
			params.subresourceRange = regions.subresource;
			params.image = core::smart_refctd_ptr(image);

			m_fontAtlasTexture = m_creationParams.utilities->getLogicalDevice()->createImageView(std::move(params));
		}
		
        ISemaphore::future_t<IQueue::RESULT> retval(IQueue::RESULT::SUCCESS);
        retval.set({sInfo.scratchSemaphore.semaphore,sInfo.scratchSemaphore.value});
        return retval;
	}

	void UI::handleMouseEvents(const S_UPDATE_PARAMETERS& params) const
	{
		auto& io = ImGui::GetIO();

		io.AddMousePosEvent(params.mousePosition.x, params.mousePosition.y);

		for (const auto& e : params.events.mouse)
		{
			switch (e.type)
			{
			case SMouseEvent::EET_CLICK:
			{
				ImGuiMouseButton_ button = ImGuiMouseButton_COUNT;
				if (e.clickEvent.mouseButton == EMB_LEFT_BUTTON)
					button = ImGuiMouseButton_Left;
				else if (e.clickEvent.mouseButton == EMB_RIGHT_BUTTON)
					button = ImGuiMouseButton_Right;
				else if (e.clickEvent.mouseButton == EMB_MIDDLE_BUTTON)
					button = ImGuiMouseButton_Middle;

				if (button == ImGuiMouseButton_COUNT)
					continue;

				if (e.clickEvent.action == SMouseEvent::SClickEvent::EA_PRESSED)
					io.AddMouseButtonEvent(button, true);
				else if (e.clickEvent.action == SMouseEvent::SClickEvent::EA_RELEASED)
					io.AddMouseButtonEvent(button, false);
			} break;

			case SMouseEvent::EET_SCROLL:
			{
				_NBL_STATIC_INLINE_CONSTEXPR auto scalar = 0.02f;
				const auto wheel = nbl::hlsl::float32_t2(e.scrollEvent.horizontalScroll, e.scrollEvent.verticalScroll) * scalar;

				io.AddMouseWheelEvent(wheel.x, wheel.y);
			} break;

			case SMouseEvent::EET_MOVEMENT:

			default:
				break;
			}
		}
	}

	struct NBL_TO_IMGUI_KEY_BIND 
	{
		ImGuiKey target;
		char physicalSmall;
		char physicalBig;
	};

	// maps Nabla keys to IMGUIs
	_NBL_STATIC_INLINE_CONSTEXPR std::array<NBL_TO_IMGUI_KEY_BIND, EKC_COUNT> createKeyMap()
	{
		std::array<NBL_TO_IMGUI_KEY_BIND, EKC_COUNT> map = { { NBL_TO_IMGUI_KEY_BIND{ImGuiKey_None, '0', '0'} } };

		#define NBL_REGISTER_KEY(__NBL_KEY__, __IMGUI_KEY__) \
			map[__NBL_KEY__] = NBL_TO_IMGUI_KEY_BIND{__IMGUI_KEY__, keyCodeToChar(__NBL_KEY__, false), keyCodeToChar(__NBL_KEY__, true)};

		NBL_REGISTER_KEY(EKC_BACKSPACE, ImGuiKey_Backspace);
		NBL_REGISTER_KEY(EKC_TAB, ImGuiKey_Tab);
		NBL_REGISTER_KEY(EKC_ENTER, ImGuiKey_Enter);
		NBL_REGISTER_KEY(EKC_LEFT_SHIFT, ImGuiKey_LeftShift);
		NBL_REGISTER_KEY(EKC_RIGHT_SHIFT, ImGuiKey_RightShift);
		NBL_REGISTER_KEY(EKC_LEFT_CONTROL, ImGuiKey_LeftCtrl);
		NBL_REGISTER_KEY(EKC_RIGHT_CONTROL, ImGuiKey_RightCtrl);
		NBL_REGISTER_KEY(EKC_LEFT_ALT, ImGuiKey_LeftAlt);
		NBL_REGISTER_KEY(EKC_RIGHT_ALT, ImGuiKey_RightAlt);
		NBL_REGISTER_KEY(EKC_PAUSE, ImGuiKey_Pause);
		NBL_REGISTER_KEY(EKC_CAPS_LOCK, ImGuiKey_CapsLock);
		NBL_REGISTER_KEY(EKC_ESCAPE, ImGuiKey_Escape);
		NBL_REGISTER_KEY(EKC_SPACE, ImGuiKey_Space);
		NBL_REGISTER_KEY(EKC_PAGE_UP, ImGuiKey_PageUp);
		NBL_REGISTER_KEY(EKC_PAGE_DOWN, ImGuiKey_PageDown);
		NBL_REGISTER_KEY(EKC_END, ImGuiKey_End);
		NBL_REGISTER_KEY(EKC_HOME, ImGuiKey_Home);
		NBL_REGISTER_KEY(EKC_LEFT_ARROW, ImGuiKey_LeftArrow);
		NBL_REGISTER_KEY(EKC_RIGHT_ARROW, ImGuiKey_RightArrow);
		NBL_REGISTER_KEY(EKC_DOWN_ARROW, ImGuiKey_DownArrow);
		NBL_REGISTER_KEY(EKC_UP_ARROW, ImGuiKey_UpArrow);
		NBL_REGISTER_KEY(EKC_PRINT_SCREEN, ImGuiKey_PrintScreen);
		NBL_REGISTER_KEY(EKC_INSERT, ImGuiKey_Insert);
		NBL_REGISTER_KEY(EKC_DELETE, ImGuiKey_Delete);
		NBL_REGISTER_KEY(EKC_APPS, ImGuiKey_Menu);
		NBL_REGISTER_KEY(EKC_COMMA, ImGuiKey_Comma);
		NBL_REGISTER_KEY(EKC_PERIOD, ImGuiKey_Period);
		NBL_REGISTER_KEY(EKC_SEMICOLON, ImGuiKey_Semicolon);
		NBL_REGISTER_KEY(EKC_OPEN_BRACKET, ImGuiKey_LeftBracket);
		NBL_REGISTER_KEY(EKC_CLOSE_BRACKET, ImGuiKey_RightBracket);
		NBL_REGISTER_KEY(EKC_BACKSLASH, ImGuiKey_Backslash);
		NBL_REGISTER_KEY(EKC_APOSTROPHE, ImGuiKey_Apostrophe);
		NBL_REGISTER_KEY(EKC_ADD, ImGuiKey_KeypadAdd);
		NBL_REGISTER_KEY(EKC_SUBTRACT, ImGuiKey_KeypadSubtract);
		NBL_REGISTER_KEY(EKC_MULTIPLY, ImGuiKey_KeypadMultiply);
		NBL_REGISTER_KEY(EKC_DIVIDE, ImGuiKey_KeypadDivide);
		NBL_REGISTER_KEY(EKC_0, ImGuiKey_0);
		NBL_REGISTER_KEY(EKC_1, ImGuiKey_1);
		NBL_REGISTER_KEY(EKC_2, ImGuiKey_2);
		NBL_REGISTER_KEY(EKC_3, ImGuiKey_3);
		NBL_REGISTER_KEY(EKC_4, ImGuiKey_4);
		NBL_REGISTER_KEY(EKC_5, ImGuiKey_5);
		NBL_REGISTER_KEY(EKC_6, ImGuiKey_6);
		NBL_REGISTER_KEY(EKC_7, ImGuiKey_7);
		NBL_REGISTER_KEY(EKC_8, ImGuiKey_8);
		NBL_REGISTER_KEY(EKC_9, ImGuiKey_9);
		NBL_REGISTER_KEY(EKC_A, ImGuiKey_A);
		NBL_REGISTER_KEY(EKC_B, ImGuiKey_B);
		NBL_REGISTER_KEY(EKC_C, ImGuiKey_C);
		NBL_REGISTER_KEY(EKC_D, ImGuiKey_D);
		NBL_REGISTER_KEY(EKC_E, ImGuiKey_E);
		NBL_REGISTER_KEY(EKC_F, ImGuiKey_F);
		NBL_REGISTER_KEY(EKC_G, ImGuiKey_G);
		NBL_REGISTER_KEY(EKC_H, ImGuiKey_H);
		NBL_REGISTER_KEY(EKC_I, ImGuiKey_I);
		NBL_REGISTER_KEY(EKC_J, ImGuiKey_J);
		NBL_REGISTER_KEY(EKC_K, ImGuiKey_K);
		NBL_REGISTER_KEY(EKC_L, ImGuiKey_L);
		NBL_REGISTER_KEY(EKC_M, ImGuiKey_M);
		NBL_REGISTER_KEY(EKC_N, ImGuiKey_N);
		NBL_REGISTER_KEY(EKC_O, ImGuiKey_O);
		NBL_REGISTER_KEY(EKC_P, ImGuiKey_P);
		NBL_REGISTER_KEY(EKC_Q, ImGuiKey_Q);
		NBL_REGISTER_KEY(EKC_R, ImGuiKey_R);
		NBL_REGISTER_KEY(EKC_S, ImGuiKey_S);
		NBL_REGISTER_KEY(EKC_T, ImGuiKey_T);
		NBL_REGISTER_KEY(EKC_U, ImGuiKey_U);
		NBL_REGISTER_KEY(EKC_V, ImGuiKey_V);
		NBL_REGISTER_KEY(EKC_W, ImGuiKey_W);
		NBL_REGISTER_KEY(EKC_X, ImGuiKey_X);
		NBL_REGISTER_KEY(EKC_Y, ImGuiKey_Y);
		NBL_REGISTER_KEY(EKC_Z, ImGuiKey_Z);
		NBL_REGISTER_KEY(EKC_NUMPAD_0, ImGuiKey_Keypad0);
		NBL_REGISTER_KEY(EKC_NUMPAD_1, ImGuiKey_Keypad1);
		NBL_REGISTER_KEY(EKC_NUMPAD_2, ImGuiKey_Keypad2);
		NBL_REGISTER_KEY(EKC_NUMPAD_3, ImGuiKey_Keypad3);
		NBL_REGISTER_KEY(EKC_NUMPAD_4, ImGuiKey_Keypad4);
		NBL_REGISTER_KEY(EKC_NUMPAD_5, ImGuiKey_Keypad5);
		NBL_REGISTER_KEY(EKC_NUMPAD_6, ImGuiKey_Keypad6);
		NBL_REGISTER_KEY(EKC_NUMPAD_7, ImGuiKey_Keypad7);
		NBL_REGISTER_KEY(EKC_NUMPAD_8, ImGuiKey_Keypad8);
		NBL_REGISTER_KEY(EKC_NUMPAD_9, ImGuiKey_Keypad9);
		NBL_REGISTER_KEY(EKC_F1, ImGuiKey_F1);
		NBL_REGISTER_KEY(EKC_F2, ImGuiKey_F2);
		NBL_REGISTER_KEY(EKC_F3, ImGuiKey_F3);
		NBL_REGISTER_KEY(EKC_F4, ImGuiKey_F4);
		NBL_REGISTER_KEY(EKC_F5, ImGuiKey_F5);
		NBL_REGISTER_KEY(EKC_F6, ImGuiKey_F6);
		NBL_REGISTER_KEY(EKC_F7, ImGuiKey_F7);
		NBL_REGISTER_KEY(EKC_F8, ImGuiKey_F8);
		NBL_REGISTER_KEY(EKC_F9, ImGuiKey_F9);
		NBL_REGISTER_KEY(EKC_F10, ImGuiKey_F10);
		NBL_REGISTER_KEY(EKC_F11, ImGuiKey_F11);
		NBL_REGISTER_KEY(EKC_F12, ImGuiKey_F12);
		NBL_REGISTER_KEY(EKC_F13, ImGuiKey_F13);
		NBL_REGISTER_KEY(EKC_F14, ImGuiKey_F14);
		NBL_REGISTER_KEY(EKC_F15, ImGuiKey_F15);
		NBL_REGISTER_KEY(EKC_F16, ImGuiKey_F16);
		NBL_REGISTER_KEY(EKC_F17, ImGuiKey_F17);
		NBL_REGISTER_KEY(EKC_F18, ImGuiKey_F18);
		NBL_REGISTER_KEY(EKC_F19, ImGuiKey_F19);
		NBL_REGISTER_KEY(EKC_F20, ImGuiKey_F20);
		NBL_REGISTER_KEY(EKC_F21, ImGuiKey_F21);
		NBL_REGISTER_KEY(EKC_F22, ImGuiKey_F22);
		NBL_REGISTER_KEY(EKC_F23, ImGuiKey_F23);
		NBL_REGISTER_KEY(EKC_F24, ImGuiKey_F24);
		NBL_REGISTER_KEY(EKC_NUM_LOCK, ImGuiKey_NumLock);
		NBL_REGISTER_KEY(EKC_SCROLL_LOCK, ImGuiKey_ScrollLock);
		NBL_REGISTER_KEY(EKC_VOLUME_MUTE, ImGuiKey_None);
		NBL_REGISTER_KEY(EKC_VOLUME_UP, ImGuiKey_None);
		NBL_REGISTER_KEY(EKC_VOLUME_DOWN, ImGuiKey_None);

		return map;
	}

	void UI::handleKeyEvents(const S_UPDATE_PARAMETERS& params) const
	{
		auto& io = ImGui::GetIO();

		_NBL_STATIC_INLINE_CONSTEXPR auto keyMap = createKeyMap();

		const bool useBigLetters = [&]()  // TODO: we can later improve it to check for CAPS, etc
		{
			for (const auto& e : params.events.keyboard)
				if (e.keyCode == EKC_LEFT_SHIFT && e.action == SKeyboardEvent::ECA_PRESSED)
					return true;

			return false;
		}();

		for (const auto& e : params.events.keyboard)
		{
			const auto& bind = keyMap[e.keyCode];
			const auto& iCharacter = useBigLetters ? bind.physicalBig : bind.physicalSmall;

			if(bind.target == ImGuiKey_None)
				m_creationParams.utilities->getLogger()->log(std::string("Requested physical Nabla key \"") + iCharacter + std::string("\" has yet no mapping to IMGUI key!"), system::ILogger::ELL_ERROR);
			else
				if (e.action == SKeyboardEvent::ECA_PRESSED)
				{
					io.AddKeyEvent(bind.target, true);
					io.AddInputCharacter(iCharacter);
				}
				else if (e.action == SKeyboardEvent::ECA_RELEASED)
					io.AddKeyEvent(bind.target, false);
		}
	}

	UI::UI(S_CREATION_PARAMETERS&& params)
		: m_creationParams(std::move(params))
	{
		auto validateResourcesInfo = [&]() -> bool
		{
			auto* pipelineLayout = m_creationParams.resources.pipelineLayout;

			if (pipelineLayout) // provided? we will validate your pipeline layout to check if you declared required UI resources
			{
				auto validateResource = [&]<IDescriptor::E_TYPE descriptorType>(const IGPUDescriptorSetLayout* const descriptorSetLayout)
				{
					constexpr std::string_view typeLiteral = descriptorType == IDescriptor::E_TYPE::ET_SAMPLED_IMAGE ? "ET_SAMPLED_IMAGE" : "ET_SAMPLER",
					ixLiteral = descriptorType == IDescriptor::E_TYPE::ET_SAMPLED_IMAGE ? "texturesBindingIx" : "samplersBindingIx";

					auto anyBindingCount = [&m_creationParams = m_creationParams, &log = std::as_const(typeLiteral)](const IDescriptorSetLayoutBase::CBindingRedirect* redirect) -> bool
					{
						if (redirect->getBindingCount())
						{
							m_creationParams.utilities->getLogger()->log("Provided descriptor set layout has no bindings for IDescriptor::E_TYPE::%s, you are required to provide at least single default ImGUI Font Atlas texture resource & corresponsing sampler resource!", system::ILogger::ELL_ERROR, log.data());
							return false;
						}

						return true;
					};

					if(!descriptorSetLayout)
					{
						m_creationParams.utilities->getLogger()->log("Provided descriptor set layout for IDescriptor::E_TYPE::%s is nullptr!", system::ILogger::ELL_ERROR, typeLiteral.data());
						return false;
					}

					const auto* redirect = &descriptorSetLayout->getDescriptorRedirect(descriptorType);

					if constexpr (descriptorType == IDescriptor::E_TYPE::ET_SAMPLED_IMAGE)
						if (!anyBindingCount(redirect))
							return false;
					else
					{
						if (!anyBindingCount(redirect))
						{
							redirect = &descriptorSetLayout->getImmutableSamplerRedirect(); // we must give it another try & request to look for immutable samplers

							if (!anyBindingCount(redirect))
								return false;
						}
					}

					const auto bindingCount = redirect->getBindingCount();

					bool ok = false;
					for (uint32_t i = 0u; i < bindingCount; ++i)
					{
						const auto rangeStorageIndex = IDescriptorSetLayoutBase::CBindingRedirect::storage_range_index_t(i);
						const auto binding = redirect->getBinding(rangeStorageIndex);
						const auto requestedBindingIx = descriptorType == IDescriptor::E_TYPE::ET_SAMPLED_IMAGE ? m_creationParams.resources.textures.bindingIx : m_creationParams.resources.samplers.bindingIx;

						if (binding.data == requestedBindingIx)
						{
							const auto count = redirect->getCount(binding);

							if (count != m_creationParams.resources.count)
							{
								m_creationParams.utilities->getLogger()->log("Provided descriptor set layout has IDescriptor::E_TYPE::%s binding for requested `m_creationParams.resources.%s` index but with different binding count!", system::ILogger::ELL_ERROR, typeLiteral.data(), ixLiteral.data());
								return false;
							}

							const auto stage = redirect->getStageFlags(binding);

							if(!stage.hasFlags(m_creationParams.resources.RESOURCES_REQUIRED_STAGE_FLAGS))
							{
								m_creationParams.utilities->getLogger()->log("Provided descriptor set layout has IDescriptor::E_TYPE::%s binding for requested `m_creationParams.resources.%s` index but doesn't meet stage flags requirements!", system::ILogger::ELL_ERROR, typeLiteral.data(), ixLiteral.data());
								return false;
							}

							const auto create = redirect->getCreateFlags(rangeStorageIndex);

							if (!create.hasFlags(descriptorType == IDescriptor::E_TYPE::ET_SAMPLED_IMAGE ? m_creationParams.resources.TEXTURES_REQUIRED_CREATE_FLAGS : m_creationParams.resources.SAMPLERS_REQUIRED_CREATE_FLAGS))
							{
								m_creationParams.utilities->getLogger()->log("Provided descriptor set layout has IDescriptor::E_TYPE::%s binding for requested `m_creationParams.resources.%s` index but doesn't meet create flags requirements!", system::ILogger::ELL_ERROR, typeLiteral.data(), ixLiteral.data());
								return false;
							}

							ok = true;
							break;
						}
					}

					if (!ok)
					{
						m_creationParams.utilities->getLogger()->log("Provided descriptor set layout has no IDescriptor::E_TYPE::%s binding for requested `m_creationParams.resources.%s` index or it is invalid!", system::ILogger::ELL_ERROR, typeLiteral.data(), ixLiteral.data());
						return false;
					}

					return true;
				};

				const auto& layouts = pipelineLayout->getDescriptorSetLayouts();
				const bool ok = validateResource.template operator() < IDescriptor::E_TYPE::ET_SAMPLED_IMAGE > (layouts[m_creationParams.resources.textures.setIx]) && validateResource.template operator() < IDescriptor::E_TYPE::ET_SAMPLER > (layouts[m_creationParams.resources.samplers.setIx]);

				if (!ok)
					return false;
			}

			return true;
		};

		const auto validation = std::to_array
		({
			std::make_pair(bool(m_creationParams.assetManager), "Invalid `m_creationParams.assetManager` is nullptr!"),
			std::make_pair(bool(m_creationParams.assetManager->getSystem()), "Invalid `m_creationParams.assetManager->getSystem()` is nullptr!"),
			std::make_pair(bool(m_creationParams.utilities), "Invalid `m_creationParams.utilities` is nullptr!"),
			std::make_pair(bool(m_creationParams.transfer), "Invalid `m_creationParams.transfer` is nullptr!"),
			std::make_pair(bool(m_creationParams.renderpass), "Invalid `m_creationParams.renderpass` is nullptr!"),
			(m_creationParams.assetManager && m_creationParams.utilities && m_creationParams.transfer && m_creationParams.renderpass) ? std::make_pair(bool(m_creationParams.utilities->getLogicalDevice()->getPhysicalDevice()->getQueueFamilyProperties()[m_creationParams.transfer->getFamilyIndex()].queueFlags.hasFlags(IQueue::FAMILY_FLAGS::TRANSFER_BIT)), "Invalid `m_creationParams.transfer` is not capable of transfer operations!") : std::make_pair(false, "Pass valid required UI::S_CREATION_PARAMETERS!"),
			std::make_pair(bool(m_creationParams.resources.count >= 1u), "Invalid `m_creationParams.resources.count` is equal to 0!"),
			std::make_pair(bool(m_creationParams.resources.textures.setIx <= 3u), "Invalid `m_creationParams.resources.textures` is outside { 0u, 1u, 2u, 3u } set!"),
			std::make_pair(bool(m_creationParams.resources.samplers.setIx <= 3u), "Invalid `m_creationParams.resources.samplers` is outside { 0u, 1u, 2u, 3u } set!"),
			std::make_pair(bool(m_creationParams.resources.textures.bindingIx != m_creationParams.resources.samplers.bindingIx), "Invalid `m_creationParams.resources.textures.bindingIx` is equal to `m_creationParams.resources.samplers.bindingIx`!"),
			std::make_pair(bool(validateResourcesInfo()), "Invalid `m_creationParams.resources` content!")
		});

		for (const auto& [ok, error] : validation)
			if (!ok)
			{
				m_creationParams.utilities->getLogger()->log(error, system::ILogger::ELL_ERROR);
				assert(false);
			}

		smart_refctd_ptr<nbl::video::IGPUCommandBuffer> transistentCMD;
		{
			using pool_flags_t = IGPUCommandPool::CREATE_FLAGS;

			smart_refctd_ptr<nbl::video::IGPUCommandPool> pool = m_creationParams.utilities->getLogicalDevice()->createCommandPool(m_creationParams.transfer->getFamilyIndex(), pool_flags_t::RESET_COMMAND_BUFFER_BIT|pool_flags_t::TRANSIENT_BIT);
			if (!pool)
			{
				m_creationParams.utilities->getLogger()->log("Could not create command pool!", system::ILogger::ELL_ERROR);
				assert(false);
			}
			
			if (!pool->createCommandBuffers(IGPUCommandPool::BUFFER_LEVEL::PRIMARY, 1u, &transistentCMD))
			{
				m_creationParams.utilities->getLogger()->log("Could not create transistent command buffer!", system::ILogger::ELL_ERROR);
				assert(false);
			}
		}

		// Dear ImGui context
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();

		createPipeline();
		createMDIBuffer();
		createFontAtlasTexture(transistentCMD.get());

		auto & io = ImGui::GetIO();
		io.BackendUsingLegacyKeyArrays = 0; // using AddKeyEvent() - it's new way of handling ImGUI events our backends supports
	}

	UI::~UI() = default;

	void UI::createMDIBuffer()
	{
		constexpr static uint32_t minStreamingBufferAllocationSize = 32u, maxStreamingBufferAllocationAlignment = 1024u * 64u, mdiBufferDefaultSize = /* 2MB */ 1024u * 1024u * 2u;

		auto getRequiredAccessFlags = [&](const core::bitflag<video::IDeviceMemoryAllocation::E_MEMORY_PROPERTY_FLAGS>& properties)
		{
			core::bitflag<IDeviceMemoryAllocation::E_MAPPING_CPU_ACCESS_FLAGS> flags (IDeviceMemoryAllocation::EMCAF_NO_MAPPING_ACCESS);

			if (properties.hasFlags(IDeviceMemoryAllocation::EMPF_HOST_READABLE_BIT))
				flags |= IDeviceMemoryAllocation::EMCAF_READ;
			if (properties.hasFlags(IDeviceMemoryAllocation::EMPF_HOST_WRITABLE_BIT))
				flags |= IDeviceMemoryAllocation::EMCAF_WRITE;

			return flags;
		};

		if (m_creationParams.streamingBuffer)
			m_mdi.buffer = core::smart_refctd_ptr<typename MDI::COMPOSE_T>(m_creationParams.streamingBuffer);
		else
		{
			IGPUBuffer::SCreationParams mdiCreationParams = {};
			mdiCreationParams.usage = MDI::MDI_BUFFER_REQUIRED_USAGE_FLAGS;
			mdiCreationParams.size = mdiBufferDefaultSize;

			auto buffer = m_creationParams.utilities->getLogicalDevice()->createBuffer(std::move(mdiCreationParams));
			buffer->setObjectDebugName("MDI Upstream Buffer");

			auto memoryReqs = buffer->getMemoryReqs();
			memoryReqs.memoryTypeBits &= m_creationParams.utilities->getLogicalDevice()->getPhysicalDevice()->getUpStreamingMemoryTypeBits();

			auto allocation = m_creationParams.utilities->getLogicalDevice()->allocate(memoryReqs, buffer.get(), MDI::MDI_BUFFER_REQUIRED_ALLOCATE_FLAGS);
			{
				const bool allocated = allocation.isValid();
				assert(allocated);
			}
			auto memory = allocation.memory;

			if (!memory->map({ 0ull, memoryReqs.size }, getRequiredAccessFlags(memory->getMemoryPropertyFlags())))
				m_creationParams.utilities->getLogger()->log("Could not map device memory!", system::ILogger::ELL_ERROR);

			m_mdi.buffer = std::move(buffer);
		}

		auto buffer = m_mdi.buffer;
		auto binding = buffer->getBoundMemory();

		constexpr auto ALIGN_OFFSET = 0u, MIN_BLOCK_SIZE = 1024u;
		m_mdi.allocator = MDI::ALLOCATOR_TRAITS_T::allocator_type(binding.memory->getMappedPointer(), binding.offset, ALIGN_OFFSET, MDI_MAX_ALIGNMENT, binding.memory->getAllocationSize(), MIN_BLOCK_SIZE);

		const auto validation = std::to_array
		({
			std::make_pair(buffer->getCreationParams().usage.hasFlags(MDI::MDI_BUFFER_REQUIRED_USAGE_FLAGS), "MDI buffer must be created with IBuffer::EUF_INDIRECT_BUFFER_BIT | IBuffer::EUF_INDEX_BUFFER_BIT | IBuffer::EUF_VERTEX_BUFFER_BIT | IBuffer::EUF_SHADER_DEVICE_ADDRESS_BIT enabled!"),
			std::make_pair(bool(buffer->getMemoryReqs().memoryTypeBits & m_creationParams.utilities->getLogicalDevice()->getPhysicalDevice()->getUpStreamingMemoryTypeBits()), "MDI buffer must have up-streaming memory type bits enabled!"),
			std::make_pair(binding.memory->getAllocateFlags().hasFlags(MDI::MDI_BUFFER_REQUIRED_ALLOCATE_FLAGS), "MDI buffer's memory must be allocated with IDeviceMemoryAllocation::EMAF_DEVICE_ADDRESS_BIT enabled!"),
			std::make_pair(binding.memory->isCurrentlyMapped(), "MDI buffer's memory must be mapped!"), // streaming buffer contructor already validates it, but cannot assume user won't unmap its own buffer for some reason (sorry if you have just hit it)
			std::make_pair(binding.memory->getCurrentMappingAccess().hasFlags(getRequiredAccessFlags(binding.memory->getMemoryPropertyFlags())), "MDI buffer's memory current mapping access flags don't meet requirements!")
		});

		for (const auto& [ok, error] : validation)
			if (!ok)
			{
				m_creationParams.utilities->getLogger()->log(error, system::ILogger::ELL_ERROR);
				assert(false);
			}
	}

	template<typename T>
	concept ImDrawBufferType = std::same_as<T, ImDrawVert> || std::same_as<T, ImDrawIdx>;

	bool UI::render(nbl::video::IGPUCommandBuffer* commandBuffer, const std::span<const VkRect2D> scissors)
	{
		if (!commandBuffer)
		{
			m_creationParams.utilities->getLogger()->log("Invalid command buffer!", system::ILogger::ELL_ERROR);
			return false;
		}

		if (commandBuffer->getState() != IGPUCommandBuffer::STATE::RECORDING)
		{
			m_creationParams.utilities->getLogger()->log("Command buffer is not in recording state!", system::ILogger::ELL_ERROR);
			return false;
		}

		ImGui::Render(); // note it doesn't touch GPU or graphics API at all, its an internal ImGUI call to update & prepare the data for rendering so we can call GetDrawData()

		ImGuiIO& io = ImGui::GetIO();

		if (!io.Fonts->IsBuilt())
		{
			m_creationParams.utilities->getLogger()->log("Font atlas not built! It is generally built by the renderer backend. Missing call to renderer _NewFrame() function? e.g. ImGui_ImplOpenGL3_NewFrame().", system::ILogger::ELL_ERROR);
			return false;
		}
		
		auto const* drawData = ImGui::GetDrawData();

		if (!drawData)
			return false;
		
		// Avoid rendering when minimized, scale coordinates for retina displays (screen coordinates != framebuffer coordinates)
		float const frameBufferWidth = drawData->DisplaySize.x * drawData->FramebufferScale.x;
		float const frameBufferHeight = drawData->DisplaySize.y * drawData->FramebufferScale.y;
		if (frameBufferWidth > 0 && frameBufferHeight > 0 && drawData->TotalVtxCount > 0)
		{
			const struct
			{
				ImVec2 off;			// (0,0) unless using multi-viewports
				ImVec2 scale;		// (1,1) unless using retina display which are often (2,2)
				ImVec2 framebuffer; // width, height

				// Project scissor/clipping rectangles into frame-buffer space
				ImVec4 getClipRectangle(const ImDrawCmd* cmd) const
				{
					assert(cmd);

					ImVec4 rectangle;
					rectangle.x = (cmd->ClipRect.x - off.x) * scale.x;
					rectangle.y = (cmd->ClipRect.y - off.y) * scale.y;
					rectangle.z = (cmd->ClipRect.z - off.x) * scale.x;
					rectangle.w = (cmd->ClipRect.w - off.y) * scale.y;

					return rectangle;
				}

				VkRect2D getScissor(ImVec4 clipRectangle) const
				{
					// Negative offsets are illegal for vkCmdSetScissor
					if (clipRectangle.x < 0.0f)
						clipRectangle.x = 0.0f;
					if (clipRectangle.y < 0.0f)
						clipRectangle.y = 0.0f;

					{// Apply scissor/clipping rectangle
						VkRect2D scissor {};
						scissor.offset.x = static_cast<int32_t>(clipRectangle.x);
						scissor.offset.y = static_cast<int32_t>(clipRectangle.y);
						scissor.extent.width = static_cast<uint32_t>(clipRectangle.z - clipRectangle.x);
						scissor.extent.height = static_cast<uint32_t>(clipRectangle.w - clipRectangle.y);
						
						return scissor;
					}
				}
			} clip { .off = drawData->DisplayPos, .scale = drawData->FramebufferScale, .framebuffer = { frameBufferWidth, frameBufferHeight } };
			
			struct TRS
			{
				core::vector2df_SIMD scale;
				core::vector2df_SIMD translate;

				core::vector2df_SIMD toNDC(core::vector2df_SIMD in) const
				{
					return in * scale + translate;
				}
			};

			const TRS trs = [&]() 
			{
				TRS retV;

				retV.scale = core::vector2df_SIMD{ 2.0f / drawData->DisplaySize.x , 2.0f / drawData->DisplaySize.y };
				retV.translate = core::vector2df_SIMD { -1.0f, -1.0f } - core::vector2df_SIMD{ drawData->DisplayPos.x, drawData->DisplayPos.y } * trs.scale;

				return std::move(retV);
			}();

			struct MDI_PARAMS
			{
				std::array<typename MDI::ALLOCATOR_TRAITS_T::size_type, MDI_COMPONENT_COUNT> bytesToFill = {}; //! used with MDI::E_BUFFER_CONTENT for elements
				typename MDI::ALLOCATOR_TRAITS_T::size_type totalByteSizeRequest = {}, //! sum of bytesToFill
				drawCount = {};	//! amount of indirect draw objects
			};

			const MDI_PARAMS mdiParams = [&]()
			{
				MDI_PARAMS params;

				for (uint32_t i = 0; i < drawData->CmdListsCount; i++)
				{
					const ImDrawList* commandList = drawData->CmdLists[i];

					// calculate upper bound byte size for each mdi's content
					params.bytesToFill[MDI::EBC_DRAW_INDIRECT_STRUCTURES] += commandList->CmdBuffer.Size * sizeof(VkDrawIndexedIndirectCommand);
					params.bytesToFill[MDI::EBC_ELEMENT_STRUCTURES] += commandList->CmdBuffer.Size * sizeof(PerObjectData);
					params.bytesToFill[MDI::EBC_INDEX_BUFFERS] += commandList->IdxBuffer.Size * sizeof(ImDrawIdx);
					params.bytesToFill[MDI::EBC_VERTEX_BUFFERS] += commandList->VtxBuffer.Size * sizeof(ImDrawVert);
				}

				// calculate upper bound byte size limit for mdi buffer
				params.totalByteSizeRequest = std::reduce(std::begin(params.bytesToFill), std::end(params.bytesToFill));
				params.drawCount = params.bytesToFill[MDI::EBC_DRAW_INDIRECT_STRUCTURES] / sizeof(VkDrawIndexedIndirectCommand);

				return std::move(params);
			}();

			std::array<bool, MDI_COMPONENT_COUNT> mdiBytesFilled;
			std::fill(mdiBytesFilled.data(), mdiBytesFilled.data() + MDI_COMPONENT_COUNT, false);
			std::array<typename MDI::ALLOCATOR_TRAITS_T::size_type, MDI_COMPONENT_COUNT> mdiOffsets;
			std::fill(mdiOffsets.data(), mdiOffsets.data() + MDI_COMPONENT_COUNT, MDI::ALLOCATOR_TRAITS_T::allocator_type::invalid_address);

			auto mdiBuffer = m_mdi.buffer;
			{
				auto binding = mdiBuffer->getBoundMemory();
				assert(binding.memory->isCurrentlyMapped());

				auto* const mdiData = reinterpret_cast<uint8_t*>(binding.memory->getMappedPointer()) + binding.offset;

				struct
				{
					typename MDI::ALLOCATOR_TRAITS_T::size_type offset = MDI::ALLOCATOR_TRAITS_T::allocator_type::invalid_address,
					alignment = MDI_MAX_ALIGNMENT,
					multiAllocationSize = {};
				} requestState;

				const auto start = std::chrono::steady_clock::now();
				float blockRequestFactor = 1.f;
				requestState.multiAllocationSize = m_mdi.allocator.max_size(); // first we will try with single allocation request given max free block size from the allocator

				//! we must upload entire MDI buffer data to our streaming buffer, but we cannot guarantee allocation can be done in single request
				for (typename MDI::ALLOCATOR_TRAITS_T::size_type uploadedSize = 0ull; uploadedSize < mdiParams.totalByteSizeRequest;)
				{
					auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

					if (elapsed.count() >= 1u)
						return false;

					const auto leftSizeToUpload = mdiParams.totalByteSizeRequest - uploadedSize,
					maxTotalFreeBlockSizeToAlloc = m_mdi.allocator.max_size();
					
					MDI::ALLOCATOR_TRAITS_T::multi_alloc_addr(m_mdi.allocator, 1u, &requestState.offset, &requestState.multiAllocationSize, &requestState.alignment);

					if (requestState.offset == MDI::ALLOCATOR_TRAITS_T::allocator_type::invalid_address)
					{
						requestState.multiAllocationSize = (blockRequestFactor *= 0.5f); // failed? our following allocation reqest will be half of the previous 
						continue;
					}	
					else
					{
						static constexpr auto ALIGN_OFFSET_NEEDED = 0u;
						MDI::SUBALLOCATOR_TRAITS_T::allocator_type fillSubAllocator(mdiData, requestState.offset, ALIGN_OFFSET_NEEDED, MDI_MAX_ALIGNMENT, requestState.multiAllocationSize);

						std::array<typename MDI::ALLOCATOR_TRAITS_T::size_type, MDI_COMPONENT_COUNT> offsets;
						std::fill(offsets.data(), offsets.data() + MDI_COMPONENT_COUNT, MDI::ALLOCATOR_TRAITS_T::allocator_type::invalid_address);
						MDI::SUBALLOCATOR_TRAITS_T::multi_alloc_addr(fillSubAllocator, offsets.size(), offsets.data(), mdiParams.bytesToFill.data(), MDI_ALIGNMENTS.data());

						//! linear allocator is used to fill the mdi data within suballocation memory range, 
						//! there are a few restrictions regarding how MDI::E_BUFFER_CONTENT(s) can be packed,
						//! and we have really 2 options how data can be allocated:
						//! - tightly packed data in single request covering all MDI::E_BUFFER_CONTENT within the allocated memory range
						//! - each of MDI::E_BUFFER_CONTENT allocated separately & each tightly packed

						auto fillDrawBuffers = [&]<MDI::E_BUFFER_CONTENT type>()
						{
							const typename MDI::ALLOCATOR_TRAITS_T::size_type globalBlockOffset = offsets[type];

							if (globalBlockOffset == MDI::ALLOCATOR_TRAITS_T::allocator_type::invalid_address or mdiBytesFilled[type])
								return 0u;

							auto* data = mdiData + globalBlockOffset;

							for (int n = 0; n < drawData->CmdListsCount; n++)
							{
								auto* cmd_list = drawData->CmdLists[n];

								if constexpr (type == MDI::EBC_INDEX_BUFFERS)
								{
									const auto localInputBlockOffset = cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx);
									::memcpy(data, cmd_list->IdxBuffer.Data, localInputBlockOffset);
									data += localInputBlockOffset;
								}
								else if (type == MDI::EBC_VERTEX_BUFFERS)
								{
									const auto localInputBlockOffset = cmd_list->VtxBuffer.Size * sizeof(ImDrawVert);
									::memcpy(data, cmd_list->VtxBuffer.Data, localInputBlockOffset);
									data += localInputBlockOffset;
								}
							}

							mdiBytesFilled[type] = true;
							mdiOffsets[type] = globalBlockOffset;
							return mdiParams.bytesToFill[type];
						};

						auto fillIndirectStructures = [&]<MDI::E_BUFFER_CONTENT type>()
						{
							const typename MDI::ALLOCATOR_TRAITS_T::size_type globalBlockOffset = offsets[type];

							if (globalBlockOffset == MDI::ALLOCATOR_TRAITS_T::allocator_type::invalid_address or mdiBytesFilled[type])
								return 0u;

							auto* const data = mdiData + globalBlockOffset;

							size_t cmdListIndexObjectOffset = {}, cmdListVertexObjectOffset = {}, drawID = {};

							for (int n = 0; n < drawData->CmdListsCount; n++)
							{
								const auto* cmd_list = drawData->CmdLists[n];
								for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
								{
									const auto* pcmd = &cmd_list->CmdBuffer[cmd_i];

									if constexpr (type == MDI::EBC_DRAW_INDIRECT_STRUCTURES)
									{
										auto* indirect = reinterpret_cast<VkDrawIndexedIndirectCommand*>(data) + drawID;

										indirect->firstInstance = drawID; // use base instance as draw ID
										indirect->indexCount = pcmd->ElemCount;
										indirect->instanceCount = 1u;
										indirect->firstIndex = pcmd->IdxOffset + cmdListIndexObjectOffset;
										indirect->vertexOffset = pcmd->VtxOffset + cmdListVertexObjectOffset;
									}
									else if (type == MDI::EBC_ELEMENT_STRUCTURES)
									{
										auto* element = reinterpret_cast<PerObjectData*>(data) + drawID;

										const auto clipRectangle = clip.getClipRectangle(pcmd);
										const auto scissor = clip.getScissor(clipRectangle);

										auto packSnorm16 = [](float ndc) -> int16_t
										{
											return std::round<int16_t>(std::clamp(ndc, -1.0f, 1.0f) * 32767.0f); // TODO: ok encodePixels<asset::EF_R16_SNORM, double>(void* _pix, const double* _input) but iirc we have issues with our encode/decode utils
										};

										const auto vMin = trs.toNDC(core::vector2df_SIMD(scissor.offset.x, scissor.offset.y));
										const auto vMax = trs.toNDC(core::vector2df_SIMD(scissor.offset.x + scissor.extent.width, scissor.offset.y + scissor.extent.height));

										element->aabbMin.x = packSnorm16(vMin.x);
										element->aabbMin.y = packSnorm16(vMin.y);
										element->aabbMax.x = packSnorm16(vMax.x);
										element->aabbMax.y = packSnorm16(vMax.y);
										element->texId = pcmd->TextureId;
									}

									++drawID;
								}

								cmdListIndexObjectOffset += cmd_list->IdxBuffer.Size;
								cmdListVertexObjectOffset += cmd_list->VtxBuffer.Size;
							}

							mdiBytesFilled[type] = true;
							mdiOffsets[type] = globalBlockOffset;
							return mdiParams.bytesToFill[type];
						};

						//! from biggest requests to smallest
						//! TODO: I should be able to check if I have valid offsets & fire those with MT
						uploadedSize += fillDrawBuffers.template operator() < MDI::EBC_VERTEX_BUFFERS > ();
						uploadedSize += fillDrawBuffers.template operator() < MDI::EBC_INDEX_BUFFERS > ();
						uploadedSize += fillIndirectStructures.template operator() < MDI::EBC_DRAW_INDIRECT_STRUCTURES > ();
						uploadedSize += fillIndirectStructures.template operator() < MDI::EBC_ELEMENT_STRUCTURES > ();
					}

					MDI::ALLOCATOR_TRAITS_T::multi_free_addr(m_mdi.allocator, 1u, &requestState.offset, &requestState.multiAllocationSize);
				}
			}

			assert([&mdiOffsets]() -> bool
			{
				for (const auto& offset : mdiOffsets)
					if (offset == MDI::ALLOCATOR_TRAITS_T::allocator_type::invalid_address)
						return false; // we should never hit this at this point

				return true;
			}()); // debug check only

			const auto offset = mdiBuffer->getBoundMemory().offset;
			{
				const asset::SBufferBinding<const video::IGPUBuffer> binding =
				{
					.offset = mdiOffsets[MDI::EBC_INDEX_BUFFERS],
					.buffer = smart_refctd_ptr(mdiBuffer)
				};

				if (!commandBuffer->bindIndexBuffer(binding, sizeof(ImDrawIdx) == 2 ? EIT_16BIT : EIT_32BIT))
				{
					m_creationParams.utilities->getLogger()->log("Could not bind index buffer!", system::ILogger::ELL_ERROR);
					assert(false);
				}
			}

			{
				const asset::SBufferBinding<const video::IGPUBuffer> bindings[] =
				{{
					.offset = mdiOffsets[MDI::EBC_VERTEX_BUFFERS],
					.buffer = smart_refctd_ptr(mdiBuffer)
				}};

				if(!commandBuffer->bindVertexBuffers(0, 1, bindings))
				{
					m_creationParams.utilities->getLogger()->log("Could not bind vertex buffer!", system::ILogger::ELL_ERROR);
					assert(false);
				}
			}

			SViewport const viewport
			{
				.x = 0,
				.y = 0,
				.width = frameBufferWidth,
				.height = frameBufferHeight,
				.minDepth = 0.0f,
				.maxDepth = 1.0f,
			};

			commandBuffer->setViewport(0, 1, &viewport);
			{
				if (scissors.empty())
				{
					VkRect2D defaultScisors[] = { {.offset = {(int32_t)viewport.x, (int32_t)viewport.y}, .extent = {(uint32_t)viewport.width, (uint32_t)viewport.height}} };
					commandBuffer->setScissor(defaultScisors); // cover whole viewport (dynamic scissors must be set only to not throw validation errors)
				}
				else
					commandBuffer->setScissor(scissors);
			}
			
			/*
				Setup scale and translation, our visible imgui space lies from draw_data->DisplayPps (top left) to 
				draw_data->DisplayPos+data_data->DisplaySize (bottom right). DisplayPos is (0,0) for single viewport apps.
			*/

			{
				PushConstants constants
				{
					.elementBDA = { mdiBuffer->getDeviceAddress() + mdiOffsets[MDI::EBC_ELEMENT_STRUCTURES]},
					.elementCount = { mdiParams.drawCount },
					.scale = { trs.scale[0u], trs.scale[1u] },
					.translate = { trs.translate[0u], trs.translate[1u] },
					.viewport = { viewport.x, viewport.y, viewport.width, viewport.height }
				};

				commandBuffer->pushConstants(pipeline->getLayout(), IShader::E_SHADER_STAGE::ESS_VERTEX | IShader::E_SHADER_STAGE::ESS_FRAGMENT, 0u, sizeof(constants), &constants);
			}

			const asset::SBufferBinding<const video::IGPUBuffer> binding =
			{
				.offset = mdiOffsets[MDI::EBC_DRAW_INDIRECT_STRUCTURES],
				.buffer = core::smart_refctd_ptr(mdiBuffer)
			};

			commandBuffer->drawIndexedIndirect(binding, mdiParams.drawCount, sizeof(VkDrawIndexedIndirectCommand));
		}
	
		return true;
	}

	bool UI::update(const S_UPDATE_PARAMETERS& params)
	{
		auto & io = ImGui::GetIO();
	
		io.DisplaySize = ImVec2(params.displaySize.x, params.displaySize.y);

		handleMouseEvents(params);
		handleKeyEvents(params);

		ImGui::NewFrame();

		for (auto const& subscriber : m_subscribers)
			subscriber();

		return true;
	}

	size_t UI::registerListener(const std::function<void()>& listener)
	{
		assert(listener != nullptr);
		m_subscribers.emplace_back(listener);
		return m_subscribers.size() - 1u;
	}

	std::optional<size_t> UI::unregisterListener(size_t id)
	{
		if (id < m_subscribers.size())
		{
			m_subscribers.erase(m_subscribers.begin() + id);
			return id;
		}

		return std::nullopt;
	}

	void* UI::getContext()
	{
		return reinterpret_cast<void*>(ImGui::GetCurrentContext());
	}

	void UI::setContext(void* imguiContext)
	{
		ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(imguiContext));
	}
}