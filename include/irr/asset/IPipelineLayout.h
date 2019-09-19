#ifndef __IRR_I_PIPELINE_LAYOUT_H_INCLUDED__
#define __IRR_I_PIPELINE_LAYOUT_H_INCLUDED__

#include "irr/core/IReferenceCounted.h"
#include "irr/macros.h"
#include "irr/asset/ShaderCommons.h"
#include "irr/core/memory/refctd_dynamic_array.h"
#include <algorithm>
#include <array>

namespace irr {
namespace asset
{

template<typename DescLayoutType>
class IPipelineLayout
{
public:
    struct SPushConstantRange
    {
        E_SHADER_STAGE stageFlags;
        uint32_t offset;
        uint32_t size;
    };

protected:
    virtual ~IPipelineLayout() = default;

    IPipelineLayout(
        const SPushConstantRange* const _pcRangesBegin = nullptr, const SPushConstantRange* const _pcRangesEnd = nullptr,
        core::smart_refctd_ptr<DescLayoutType> _layout0 = nullptr, core::smart_refctd_ptr<DescLayoutType> _layout1 = nullptr,
        core::smart_refctd_ptr<DescLayoutType> _layout2 = nullptr, core::smart_refctd_ptr<DescLayoutType> _layout3 = nullptr
    ) : m_descSetLayouts{{_layout0, _layout1, _layout2, _layout3}}, 
        m_pushConstantRanges(_pcRangesBegin==_pcRangesEnd ? nullptr : core::make_refctd_dynamic_array<core::smart_refctd_dynamic_array<SPushConstantRange>>(_pcRangesEnd-_pcRangesBegin))
    {
        std::copy(_pcRangesBegin, _pcRangesEnd, m_pushConstantRanges->begin());
    }

    const DescLayoutType* getDescriptorSetLayout(uint32_t _set) const { return m_descSetLayouts[_set]; }
    const core::refctd_dynamic_array<SPushConstantRange>* getPushConstantRanges() const { return m_pushConstantRanges.get(); }

    std::array<core::smart_refctd_ptr<DescLayoutType>, 4> m_descSetLayouts;
    core::smart_refctd_dynamic_array<SPushConstantRange> m_pushConstantRanges;
};

}
}

#endif