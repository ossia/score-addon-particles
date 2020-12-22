#include "ParticleNode.hpp"
#include <score/tools/Debug.hpp>

const int m_materialSize = 16;
const int instances = 5'000'000;

struct InstancedMesh : Mesh
{
  InstancedMesh(gsl::span<const float> vtx, int count)
  {
    vertexInputBindings.push_back({3 * sizeof(float)});
    vertexInputBindings.push_back({2 * sizeof(float), QRhiVertexInputBinding::Classification::PerInstance});
    vertexAttributeBindings.push_back({0, 0, QRhiVertexInputAttribute::Float3, 0});
    vertexAttributeBindings.push_back({1, 1, QRhiVertexInputAttribute::Float2, 0});
    vertexArray = vtx;
    vertexCount = count;
  }

  void setupBindings(QRhiBuffer& vtxData, QRhiBuffer* idxData, QRhiCommandBuffer& cb)
      const noexcept override
  {
  }

  const char* defaultVertexShader() const noexcept override
  {
    return R"_(#version 450
             layout(location = 0) in vec3 position;
             layout(location = 1) in vec2 offset;

             layout(location = 0) out vec3 vpos;
             layout(std140, binding = 0) uniform renderer_t {
               mat4 clipSpaceCorrMatrix;
               vec2 texcoordAdjust;
               vec2 renderSize;
             } renderer;

             out gl_PerVertex { vec4 gl_Position; };

             void main()
             {
               vpos = position;
               vec2 corr_pos = offset.xy + 0.005 * position.xy;
               corr_pos.y *= renderer.renderSize.x / renderer.renderSize.y;
               gl_Position = renderer.clipSpaceCorrMatrix * vec4(corr_pos, 0.0, 1.);
             }
         )_";
  }
};
struct InstancedTexturedCube final : InstancedMesh
{
  static const constexpr float vertices[] = {
    -1.0f,1.0f,0.0f,
    -1.0f,-1.0f,0.0f,
    1.0f,1.0f,0.0f,
    1.0f,-1.0f,0.0f,
    -1.0f,1.0f,-1.0f,
    -1.0f,-1.0f,-1.0f,
    1.0f,1.0f,-1.0f,
    1.0f,-1.0f,-1.0f
  };

  static const constexpr unsigned int indices[] = {
    0, 2, 3, 0, 3, 1,
    2, 6, 7, 2, 7, 3,
    6, 4, 5, 6, 5, 7,
    4, 0, 1, 4, 1, 5,
    0, 4, 6, 0, 6, 2,
    1, 5, 7, 1, 7, 3,
  };

  InstancedTexturedCube() : InstancedMesh{vertices, 8} {
    indexArray = indices;
    indexCount = sizeof(indices) / sizeof(unsigned int);
  }

  static const InstancedTexturedCube& instance() noexcept
  {
    static const InstancedTexturedCube t;
    return t;
  }
};
ParticuleNode::ParticuleNode()
    : m_mesh{&InstancedTexturedCube::instance()}
{
  const char* frag = R"_(#version 450
    layout(std140, binding = 0) uniform renderer_t {
      mat4 clipSpaceCorrMatrix;
      vec2 texcoordAdjust;
      vec2 renderSize;
    };

    layout(location = 0) in vec3 vpos;
    layout(location = 0) out vec4 fragColor;

    void main ()
    {
      fragColor = vec4(vpos.xy * 0.01, 0., 1.);
    }
  )_";
  std::tie(m_vertexS, m_fragmentS) = makeShaders(m_mesh->defaultVertexShader(), frag);

  m_materialData.reset(new char[m_materialSize]);
  std::fill_n(m_materialData.get(), m_materialSize, 0);

  output.push_back(new Port{this, {}, Types::Image, {}});
}

ParticuleNode::~ParticuleNode()
{

}

const Mesh& ParticuleNode::mesh() const noexcept
{
  return *this->m_mesh;
}

struct RenderedParticuleNode : score::gfx::NodeRenderer
{
  struct Pass {
    QRhiSampler* sampler{};
    TextureRenderTarget renderTarget;
    Pipeline p;
    QRhiBuffer* processUBO{};
  };
  std::array<Pass, 1> m_passes;

  //std::unique_ptr<char[]> m_materialData;

  ParticuleNode& n;



  TextureRenderTarget m_lastPassRT;

  std::vector<Sampler> m_samplers;

  QRhiBuffer* m_meshBuffer{};
  QRhiBuffer* m_idxBuffer{};
  QRhiBuffer* particleOffsets{};
  QRhiBuffer* particleSpeeds{};
  bool particlesUploaded{};

  QRhiBuffer* m_materialUBO{};
  int64_t materialChangedIndex{-1};

  QRhiComputePipeline* compute{};

  RenderedParticuleNode(const ParticuleNode& node) noexcept
    : score::gfx::NodeRenderer{}
    , n{const_cast<ParticuleNode&>(node)}
  {

  }

  virtual ~RenderedParticuleNode();
  std::optional<QSize> renderTargetSize() const noexcept override
  {
    return {};
  }

  TextureRenderTarget createRenderTarget(const RenderState& state) override
  {
    auto sz = state.size;
    if (auto true_sz = renderTargetSize())
    {
      sz = *true_sz;
    }

    m_lastPassRT = score::gfx::createRenderTarget(state, sz);
    return m_lastPassRT;
  }

  Pipeline buildPassPipeline(Renderer& renderer, TextureRenderTarget tgt, QRhiBuffer* processUBO) {

    auto buildPipeline = [] (
          const Renderer& renderer,
          const Mesh& mesh,
          const QShader& vertexS, const QShader& fragmentS,
          const TextureRenderTarget& rt,
          QRhiBuffer* m_processUBO,
          QRhiBuffer* m_materialUBO,
          const std::vector<Sampler>& samplers)
    {
      auto& rhi = *renderer.state.rhi;
      auto ps = rhi.newGraphicsPipeline();
      SCORE_ASSERT(ps);

      QRhiGraphicsPipeline::TargetBlend premulAlphaBlend;
      premulAlphaBlend.enable = true;
      premulAlphaBlend.srcColor = QRhiGraphicsPipeline::One;
      premulAlphaBlend.dstColor = QRhiGraphicsPipeline::One;
      premulAlphaBlend.opColor = QRhiGraphicsPipeline::Add;
      premulAlphaBlend.srcAlpha = QRhiGraphicsPipeline::One;
      premulAlphaBlend.dstAlpha = QRhiGraphicsPipeline::One;
      premulAlphaBlend.opAlpha = QRhiGraphicsPipeline::Add;
      ps->setTargetBlends({premulAlphaBlend});

      ps->setSampleCount(1);

      ps->setDepthTest(false);
      ps->setDepthWrite(false);

      ps->setShaderStages(
            {{QRhiShaderStage::Vertex, vertexS},
             {QRhiShaderStage::Fragment, fragmentS}});

      QRhiVertexInputLayout inputLayout;
      inputLayout.setBindings(mesh.vertexInputBindings.begin(), mesh.vertexInputBindings.end());
      inputLayout.setAttributes(
            mesh.vertexAttributeBindings.begin(), mesh.vertexAttributeBindings.end());
      ps->setVertexInputLayout(inputLayout);

      // Shader resource bindings
      auto srb = rhi.newShaderResourceBindings();
      SCORE_ASSERT(srb);

      QVector<QRhiShaderResourceBinding> bindings;

      const auto bindingStages
          = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;

      {
        const auto rendererBinding
            = QRhiShaderResourceBinding::uniformBuffer(0, bindingStages, renderer.m_rendererUBO);
        bindings.push_back(rendererBinding);
      }

      {
        const auto standardUniformBinding
            = QRhiShaderResourceBinding::uniformBuffer(1, bindingStages, m_processUBO);
        bindings.push_back(standardUniformBinding);
      }

      // Bind materials
      if (m_materialUBO)
      {
        const auto materialBinding
            = QRhiShaderResourceBinding::uniformBuffer(2, bindingStages, m_materialUBO);
        bindings.push_back(materialBinding);
      }

      // Bind samplers
      srb->setBindings(bindings.begin(), bindings.end());
      SCORE_ASSERT(srb->build());

      ps->setShaderResourceBindings(srb);

      SCORE_ASSERT(rt.renderPass);
      ps->setRenderPassDescriptor(rt.renderPass);

      SCORE_ASSERT(ps->build());
      return Pipeline{ps, srb};
    };

    return buildPipeline(renderer,  n.mesh(), n.m_vertexS, n.m_fragmentS, tgt, processUBO, m_materialUBO, m_samplers);
  };

  float data[instances * 3];
  float speed[instances * 3];
  void init(Renderer& renderer) override
  {
    QRhi& rhi = *renderer.state.rhi;

    // init()
    {
      const auto& mesh = n.mesh();
      if (!particleOffsets)
      {
        particleOffsets = rhi.newBuffer(
              QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer | QRhiBuffer::StorageBuffer, instances * 2 * sizeof(float));
        SCORE_ASSERT(particleOffsets->build());
        particleSpeeds = rhi.newBuffer(
              QRhiBuffer::Immutable, QRhiBuffer::StorageBuffer, instances * 2 * sizeof(float));
        SCORE_ASSERT(particleSpeeds->build());
      }
      if (!m_meshBuffer)
      {
        auto [mbuffer, ibuffer] = renderer.initMeshBuffer(mesh);
        m_meshBuffer = mbuffer;
        m_idxBuffer = ibuffer;
      }
    }

    if (m_materialSize > 0)
    {
      m_materialUBO
          = rhi.newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, m_materialSize);
      SCORE_ASSERT(m_materialUBO->build());
    }

    // Last pass is the main write
    {
      QRhiBuffer* pubo{};
      pubo = rhi.newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(ProcessUBO));
      pubo->build();

      auto p = buildPassPipeline(renderer, m_lastPassRT, pubo);
      m_passes[0] = Pass{nullptr, m_lastPassRT, p, pubo};
    }

    {
          QString comp = QString(R"_(#version 450
layout (local_size_x = 256) in;

struct Pos
{
    vec2 pos;
};
struct Speed
{
    vec2 spd;
};

layout(std140, binding = 0) buffer PBuf
{
    Pos d[];
} pbuf;
layout(std140, binding = 1) buffer SBuf
{
    Speed d[];
} sbuf;

void main()
{
    uint index = gl_GlobalInvocationID.x;
    if (index < %1) {
        vec2 p = pbuf.d[index].pos;
        vec2 s = sbuf.d[index].spd;

        p += s;

        pbuf.d[index].pos = p;
    }
}
)_").arg(instances);
      QShader computeShader = makeCompute(comp);
      compute = rhi.newComputePipeline();

      auto csrb = rhi.newShaderResourceBindings();
      {
        QRhiShaderResourceBinding bindings[2] = {
          QRhiShaderResourceBinding::bufferLoadStore(0, QRhiShaderResourceBinding::ComputeStage, particleOffsets),
          QRhiShaderResourceBinding::bufferLoadStore(1, QRhiShaderResourceBinding::ComputeStage, particleSpeeds)
        };

        csrb->setBindings(bindings, bindings + 2);
        SCORE_ASSERT(csrb->build());
      }
      compute->setShaderResourceBindings(csrb);
      compute->setShaderStage(QRhiShaderStage(QRhiShaderStage::Compute, computeShader));
      SCORE_ASSERT(compute->build());
    }
  }

  void update(Renderer& renderer, QRhiResourceUpdateBatch& res) override
  {
    if (m_materialUBO && m_materialSize > 0 && materialChangedIndex != n.materialChanged)
    {
      char* data = n.m_materialData.get();
      res.updateDynamicBuffer(m_materialUBO, 0, m_materialSize, data);
      materialChangedIndex = n.materialChanged;
    }

    {
      // Update all the process UBOs
      {
        n.standardUBO.passIndex = 0;
        res.updateDynamicBuffer(m_passes[0].processUBO, 0, sizeof(ProcessUBO), &this->n.standardUBO);
      }
    }


    if(!particlesUploaded) {
      for(int i = 0; i < instances * 3; i++) {
        data[i] = 2 * double(rand()) / RAND_MAX - 1;
        speed[i] = (2 * double(rand()) / RAND_MAX - 1) * 0.001;
      }

      res.uploadStaticBuffer(particleOffsets, 0, instances * 2 * sizeof(float), data);
      res.uploadStaticBuffer(particleSpeeds, 0, instances * 2 * sizeof(float), speed);
      particlesUploaded = true;
    }
  }

  void releaseWithoutRenderTarget(Renderer& r) override
  {
    {
      delete m_passes.back().p.pipeline;
      delete m_passes.back().p.srb;
      delete m_passes.back().processUBO;
    }

    for (auto sampler : m_samplers)
    {
      delete sampler.sampler;
      // texture isdeleted elsewxheree
    }
    m_samplers.clear();

    delete m_materialUBO;
    m_materialUBO = nullptr;

    m_meshBuffer = nullptr;
  }

  void release(Renderer& r) override
  {
    releaseWithoutRenderTarget(r);
    m_lastPassRT.release();
  }


  void runPass(Renderer& renderer, QRhiCommandBuffer& cb, QRhiResourceUpdateBatch& res) override
  {
    // Update a first time everything

    // PASSINDEX must be set to the last index
    // FIXME
    n.standardUBO.passIndex = m_passes.size() - 1;

    update(renderer, res);

    auto updateBatch = &res;

    // Draw the passes
    const auto& pass = m_passes[0];
    {
      SCORE_ASSERT(pass.renderTarget.renderTarget);
      SCORE_ASSERT(pass.p.pipeline);
      SCORE_ASSERT(pass.p.srb);
      // TODO : combine all the uniforms..

      auto rt = pass.renderTarget.renderTarget;
      auto pipeline = pass.p.pipeline;
      auto srb = pass.p.srb;
      auto texture = pass.renderTarget.texture;

      // TODO need to free stuff
      if(compute)
      {
        cb.beginComputePass(updateBatch);
        cb.setComputePipeline(compute);
        cb.setShaderResources(compute->shaderResourceBindings());
        cb.dispatch(instances / 256, 1, 1);
        cb.endComputePass();
      }

      cb.beginPass(rt, Qt::black, {1.0f, 0});
      {
        cb.setGraphicsPipeline(pipeline);
        cb.setShaderResources(srb);

        if(texture)
        {
          cb.setViewport(QRhiViewport(0, 0, texture->pixelSize().width(), texture->pixelSize().height()));
        }
        else
        {
          const auto sz = renderer.state.size;
          cb.setViewport(QRhiViewport(0, 0, sz.width(), sz.height()));
        }

        assert(this->m_meshBuffer);
        assert(this->m_meshBuffer->usage().testFlag(QRhiBuffer::VertexBuffer));

        const QRhiCommandBuffer::VertexInput bindings[]
            = {
          {this->m_meshBuffer, 0},
          {this->particleOffsets, 0}
        };

        cb.setVertexInput(0, 2, bindings, this->m_idxBuffer, 0, QRhiCommandBuffer::IndexFormat::IndexUInt32);

        cb.drawIndexed(n.mesh().indexCount, instances);
      }
      cb.endPass();
    }
  }
};

score::gfx::NodeRenderer* ParticuleNode::createRenderer() const noexcept
{
  return new RenderedParticuleNode{*this};
}

RenderedParticuleNode::~RenderedParticuleNode()
{

}
