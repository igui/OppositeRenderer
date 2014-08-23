/* 
 * Copyright (c) 2013 Opposite Renderer
 * For the full copyright and license information, please view the LICENSE.txt
 * file that was distributed with this source code.
*/

#include <cuda.h>
#include <curand_kernel.h>
#include "PMOptixRenderer.h"
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <limits>
#include "config.h"
#include "RandomState.h"
#include "renderer/OptixEntryPoint.h"
#include "renderer/Hitpoint.h"
#include "renderer/ppm/Photon.h"
#include "Camera.h"
#include <QThread>
#include <sstream>
#include "renderer/RayType.h"
#include "ComputeDevice.h"
#include "clientserver/RenderServerRenderRequest.h"
#include <exception>
#include "util/sutil.h"
#include "scene/IScene.h"
#include "renderer/helpers/nsight.h"

const unsigned int PMOptixRenderer::PHOTON_GRID_MAX_SIZE = 100*100*100;
const unsigned int PMOptixRenderer::MAX_PHOTON_COUNT = MAX_PHOTONS_DEPOSITS_PER_EMITTED;
const unsigned int PMOptixRenderer::PHOTON_LAUNCH_WIDTH = 512 * 2;
const unsigned int PMOptixRenderer::PHOTON_LAUNCH_HEIGHT = 512 * 2;
const unsigned int PMOptixRenderer::EMITTED_PHOTONS_PER_ITERATION = PMOptixRenderer::PHOTON_LAUNCH_WIDTH*PMOptixRenderer::PHOTON_LAUNCH_HEIGHT;
const unsigned int PMOptixRenderer::NUM_PHOTONS = PMOptixRenderer::EMITTED_PHOTONS_PER_ITERATION*PMOptixRenderer::MAX_PHOTON_COUNT;

using namespace optix;

inline unsigned int pow2roundup(unsigned int x)
{
    --x;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    return x+1;
}

inline float max(float a, float b)
{
  return a > b ? a : b;
}

PMOptixRenderer::PMOptixRenderer() : 
    m_initialized(false),
    m_width(10),
    m_height(10)
{
    try
    {
        m_context = optix::Context::create();
        if(!m_context)
        {
            throw std::exception("Unable to create OptiX context.");
        }
    }
    catch(const optix::Exception & e)
    {
        throw std::exception(e.getErrorString().c_str());
    }
    catch(const std::exception & e)
    {
        QString error = QString("Error during initialization of Optix: %1").arg(e.what());
        throw std::exception(error.toLatin1().constData());
    }
}

PMOptixRenderer::~PMOptixRenderer()
{
    m_context->destroy();
    cudaDeviceReset();
}

void PMOptixRenderer::initialize(const ComputeDevice & device, Logger *logger)
{
    if(m_initialized)
    {
        throw std::exception("ERROR: Multiple PMOptixRenderer::initialize!\n");
    }
	m_logger = logger;

    initDevice(device);

    m_context->setRayTypeCount(RayType::NUM_RAY_TYPES);
    m_context->setEntryPointCount(OptixEntryPoint::NUM_PASSES-1);
    m_context->setStackSize(1596);

    m_context["maxPhotonDepositsPerEmitted"]->setUint(MAX_PHOTON_COUNT);
    m_context["ppmAlpha"]->setFloat(0);
    m_context["totalEmitted"]->setFloat(0.0f);
    m_context["iterationNumber"]->setFloat(0.0f);
    m_context["localIterationNumber"]->setUint(0);
    m_context["ppmRadius"]->setFloat(0.f);
    m_context["ppmRadiusSquared"]->setFloat(0.f);
    m_context["emittedPhotonsPerIteration"]->setUint(EMITTED_PHOTONS_PER_ITERATION);
    m_context["emittedPhotonsPerIterationFloat"]->setFloat(float(EMITTED_PHOTONS_PER_ITERATION));
    m_context["photonLaunchWidth"]->setUint(PHOTON_LAUNCH_WIDTH);
    m_context["participatingMedium"]->setUint(0);

    // An empty scene root node
    optix::Group group = m_context->createGroup();
    m_context["sceneRootObject"]->set(group);
    
    // Display buffer

    // Ray Trace OptixEntryPoint Output Buffer

    m_raytracePassOutputBuffer = m_context->createBuffer( RT_BUFFER_INPUT_OUTPUT );
    m_raytracePassOutputBuffer->setFormat( RT_FORMAT_USER );
    m_raytracePassOutputBuffer->setElementSize( sizeof( Hitpoint ) );
    m_raytracePassOutputBuffer->setSize( m_width, m_height );
    m_context["raytracePassOutputBuffer"]->set( m_raytracePassOutputBuffer );


    // Ray OptixEntryPoint Generation Program

    {
        Program generatorProgram = m_context->createProgramFromPTXFile( "PMRayGenerator.cu.ptx", "generateRay" );
        Program exceptionProgram = m_context->createProgramFromPTXFile( "PMRayGenerator.cu.ptx", "exception" );
        Program missProgram = m_context->createProgramFromPTXFile( "PMRayGenerator.cu.ptx", "miss" );
        
        m_context->setRayGenerationProgram( OptixEntryPoint::PPM_RAYTRACE_PASS, generatorProgram );
        m_context->setExceptionProgram( OptixEntryPoint::PPM_RAYTRACE_PASS, exceptionProgram );
        m_context->setMissProgram(RayType::RADIANCE, missProgram);
        m_context->setMissProgram(RayType::RADIANCE_IN_PARTICIPATING_MEDIUM, missProgram);
    }

    //
    // Photon Tracing OptixEntryPoint
    //

    {
        Program generatorProgram = m_context->createProgramFromPTXFile( "PMPhotonGenerator.cu.ptx", "generator" );
        Program exceptionProgram = m_context->createProgramFromPTXFile( "PMPhotonGenerator.cu.ptx", "exception" );
        Program missProgram = m_context->createProgramFromPTXFile( "PMPhotonGenerator.cu.ptx", "miss");
        m_context->setRayGenerationProgram(OptixEntryPoint::PPM_PHOTON_PASS, generatorProgram);
        m_context->setMissProgram(OptixEntryPoint::PPM_PHOTON_PASS, missProgram);
        m_context->setExceptionProgram(OptixEntryPoint::PPM_PHOTON_PASS, exceptionProgram);
    }

    m_photons = m_context->createBuffer(RT_BUFFER_OUTPUT);
    m_photons->setFormat( RT_FORMAT_USER );
    m_photons->setElementSize( sizeof( Photon ) );
    m_photons->setSize( NUM_PHOTONS );
    m_context["photons"]->set( m_photons );
    m_context["photonsSize"]->setUint( NUM_PHOTONS );

    m_context["photonsGridCellSize"]->setFloat(0.0f);
    m_context["photonsGridSize"]->setUint(0,0,0);
    m_context["photonsWorldOrigo"]->setFloat(make_float3(0));
    m_photonsHashCells = m_context->createBuffer(RT_BUFFER_OUTPUT);
    m_photonsHashCells->setFormat( RT_FORMAT_UNSIGNED_INT );
    m_photonsHashCells->setSize( NUM_PHOTONS );
    m_hashmapOffsetTable = m_context->createBuffer(RT_BUFFER_OUTPUT);
    m_hashmapOffsetTable->setFormat( RT_FORMAT_UNSIGNED_INT );
    m_hashmapOffsetTable->setSize( PHOTON_GRID_MAX_SIZE+1 );
    m_context["hashmapOffsetTable"]->set( m_hashmapOffsetTable );


    //
    // Indirect Radiance Estimation Buffer
    //

    m_indirectRadianceBuffer = m_context->createBuffer( RT_BUFFER_INPUT_OUTPUT, RT_FORMAT_FLOAT3, m_width, m_height );
    m_context["indirectRadianceBuffer"]->set( m_indirectRadianceBuffer );
    
    //
    // Indirect Radiance Estimation Program
    //
    {
        Program program = m_context->createProgramFromPTXFile( "IndirectRadianceEstimation.cu.ptx", "kernel" );
        m_context->setRayGenerationProgram(OptixEntryPoint::PPM_INDIRECT_RADIANCE_ESTIMATION_PASS, program );
    }

    //
    // Direct Radiance Estimation Buffer
    //

    m_directRadianceBuffer = m_context->createBuffer( RT_BUFFER_OUTPUT, RT_FORMAT_FLOAT3, m_width, m_height );
    m_context["directRadianceBuffer"]->set( m_directRadianceBuffer );

    //
    // Direct Radiance Estimation Program
    //
    {
        Program program = m_context->createProgramFromPTXFile( "DirectRadianceEstimation.cu.ptx", "kernel" );
        m_context->setRayGenerationProgram(OptixEntryPoint::PPM_DIRECT_RADIANCE_ESTIMATION_PASS, program );
    }

    //
    // Output Buffer
    //
    {
        m_outputBuffer = m_context->createBuffer( RT_BUFFER_OUTPUT, RT_FORMAT_FLOAT3, m_width, m_height );
        m_context["outputBuffer"]->set(m_outputBuffer);
    }

    //
    // Output Program
    //
    {
        Program program = m_context->createProgramFromPTXFile( "Output.cu.ptx", "kernel" );
        m_context->setRayGenerationProgram(OptixEntryPoint::PPM_OUTPUT_PASS, program );
    }

    //
    // Random state buffer (must be large enough to give states to both photons and image pixels)
    //
  
    m_randomStatesBuffer = m_context->createBuffer(RT_BUFFER_INPUT_OUTPUT|RT_BUFFER_GPU_LOCAL);
    m_randomStatesBuffer->setFormat( RT_FORMAT_USER );
    m_randomStatesBuffer->setElementSize( sizeof( RandomState ) );
    m_randomStatesBuffer->setSize( PHOTON_LAUNCH_WIDTH, PHOTON_LAUNCH_HEIGHT );
    m_context["randomStates"]->set(m_randomStatesBuffer);

    //
    // Light sources buffer
    //

    m_lightBuffer = m_context->createBuffer(RT_BUFFER_INPUT);
    m_lightBuffer->setFormat(RT_FORMAT_USER);
    m_lightBuffer->setElementSize(sizeof(Light));
    m_lightBuffer->setSize(1);
    m_context["lights"]->set( m_lightBuffer );

    //
    // Debug buffers
    //

    createGpuDebugBuffers();


    m_initialized = true;
}

void PMOptixRenderer::initDevice(const ComputeDevice & device)
{
    // Set OptiX device as given by ComputeDevice::getDeviceId (Cuda ordinal)

    unsigned int deviceCount = m_context->getDeviceCount();
    int deviceOptixOrdinal = -1;
    for(unsigned int index = 0; index < deviceCount; ++index)
    {
        int cudaDeviceOrdinal;
        if(RTresult code = rtDeviceGetAttribute(index, RT_DEVICE_ATTRIBUTE_CUDA_DEVICE_ORDINAL, sizeof(int), &cudaDeviceOrdinal))
            throw Exception::makeException(code, 0);

        if(cudaDeviceOrdinal == device.getDeviceId())
        {
            deviceOptixOrdinal = index;
        }
    }

    m_optixDeviceOrdinal = deviceOptixOrdinal;

    if(deviceOptixOrdinal >= 0)
    {
        m_context->setDevices(&deviceOptixOrdinal, &deviceOptixOrdinal+1);
    }
    else
    {
        throw std::exception("Did not find OptiX device Number for given device. OptiX may not support this device.");
    }
}

void PMOptixRenderer::initScene( IScene & scene )
{
    if(!m_initialized)
    {
        throw std::exception("Cannot initialize scene before PMOptixRenderer.");
    }

    const QVector<Light> & lights = scene.getSceneLights();
    if(lights.size() == 0)
    {
        throw std::exception("No lights exists in this scene.");
    }

	int sceneNMeshes = scene.getNumMeshes();
	optix::Buffer hitsPerMeshBuffer = m_context->createBuffer(RT_BUFFER_INPUT_OUTPUT, RT_FORMAT_UNSIGNED_INT, sceneNMeshes);
	unsigned int* bufferHost = (unsigned int*)hitsPerMeshBuffer->map();
	memset(bufferHost, 0, sizeof(unsigned int) * sceneNMeshes);
	hitsPerMeshBuffer->unmap();
	m_context["hitsPerMeshBuffer"]->setBuffer(hitsPerMeshBuffer);
	m_context["sceneNMeshes"]->setInt(sceneNMeshes);

    try
    {
        m_sceneRootGroup = scene.getSceneRootGroup(m_context);
        m_context["sceneRootObject"]->set(m_sceneRootGroup);
        m_sceneAABB = scene.getSceneAABB();
        Sphere sceneBoundingSphere = m_sceneAABB.getBoundingSphere();
        m_context["sceneBoundingSphere"]->setUserData(sizeof(Sphere), &sceneBoundingSphere);

        // Add the lights from the scene to the light buffer

        m_lightBuffer->setSize(lights.size());
        Light* lights_host = (Light*)m_lightBuffer->map();
        memcpy(lights_host, scene.getSceneLights().constData(), sizeof(Light)*lights.size());
        m_lightBuffer->unmap();

        compile();

    }
    catch(const optix::Exception & e)
    {
        QString error = QString("An OptiX error occurred when initializing scene: %1").arg(e.getErrorString().c_str());
        throw std::exception(error.toLatin1().constData());
    }
}

void PMOptixRenderer::compile()
{
    try
    {
        m_context->validate();
        m_context->compile();
    }
    catch(const Exception& e)
    {
        throw e;
    }
}

void PMOptixRenderer::renderNextIteration(unsigned long long iterationNumber, unsigned long long localIterationNumber, float PPMRadius, 
                                        bool createOutput, const RenderServerRenderRequestDetails & details)
{
	m_logger->log("START Iteration %d\n", iterationNumber, localIterationNumber);
    if(!m_initialized)
    {
        throw std::exception("Traced before PMOptixRenderer was initialized.");
    }

	std::stringstream ss;
    ss << "PMOptixRenderer::Trace Iteration %d" << iterationNumber;
	nvtx::ScopedRange r(ss.str().c_str());

	m_logger->log("Num CPU threads: %d\n", m_context->getCPUNumThreads());
    m_logger->log("GPU paging active: %d\n", m_context->getGPUPagingActive());
    m_logger->log("Enabled devices count: %d\n", m_context->getEnabledDeviceCount());
    m_logger->log("Get devices count: %d\n", m_context->getDeviceCount());
    m_logger->log("Used host memory: %1.1fMib\n", m_context->getUsedHostMemory() / (1024.0f * 1024.0f) );
    m_logger->log("Sizeof Photon %d\n", sizeof(Photon));

	// print scene meshes count
	int sceneNMeshes = m_context["sceneNMeshes"]->getInt();
	optix::Buffer hitsPerMeshBuffer = m_context["hitsPerMeshBuffer"]->getBuffer();
	unsigned int* bufferHost = (unsigned int*)hitsPerMeshBuffer->map();
	memset(bufferHost, 0, sizeof(unsigned int) * sceneNMeshes);
	hitsPerMeshBuffer->unmap();

    try
    {
        // If the width and height of the current render request has changed, we must resize buffers
        if(details.getWidth() != m_width || details.getHeight() != m_height)
        {
            this->resizeBuffers(details.getWidth(), details.getHeight());
        }

        const Camera & camera = details.getCamera();
        const RenderMethod::E renderMethod = details.getRenderMethod();

        double traceStartTime = sutilCurrentTime();

        m_context["camera"]->setUserData( sizeof(Camera), &camera );
        m_context["iterationNumber"]->setFloat( static_cast<float>(iterationNumber));
        m_context["localIterationNumber"]->setUint((unsigned int)localIterationNumber);

        // Update PPM Radius for next photon tracing pass
        const float ppmAlpha = details.getPPMAlpha();
        m_context["ppmAlpha"]->setFloat(ppmAlpha);
        const float ppmRadiusSquared = PPMRadius*PPMRadius;
        m_context["ppmRadius"]->setFloat(PPMRadius);
        m_context["ppmRadiusSquared"]->setFloat(ppmRadiusSquared);
        const float ppmRadiusSquaredNew = ppmRadiusSquared*(iterationNumber+ppmAlpha)/float(iterationNumber+1);
        m_context["ppmRadiusSquaredNew"]->setFloat(ppmRadiusSquaredNew);


        //
        // Photon Tracing
        //

        {
			double start = sutilCurrentTime();
            nvtx::ScopedRange r( "OptixEntryPoint::PHOTON_PASS" );
            m_context->launch( OptixEntryPoint::PPM_PHOTON_PASS,
                static_cast<unsigned int>(PHOTON_LAUNCH_WIDTH),
                static_cast<unsigned int>(PHOTON_LAUNCH_HEIGHT) );

            float totalEmitted = (iterationNumber+1)*EMITTED_PHOTONS_PER_ITERATION;
            m_context["totalEmitted"]->setFloat( static_cast<float>(totalEmitted));
			double time = sutilCurrentTime() - start;
			m_logger->log("1/7 PHOTON_PASS time: %1.3fs\n", time);
        }

        debugOutputPhotonTracing();

        //
        // Create Photon Map
        //
        {
			double start = sutilCurrentTime();
            nvtx::ScopedRange r( "Creating photon map" );
			createUniformGridPhotonMap(0);
			double time = sutilCurrentTime() - start;
			m_logger->log("2/7 Creating photon map time: %1.3fs\n", time);
        }

        //
        // Transfer any data from the photon acceleration structure build to the GPU (trigger an empty launch)
        //
        {
			double start = sutilCurrentTime();
            nvtx::ScopedRange r("Transfer photon map to GPU");
            m_context->launch(OptixEntryPoint::PPM_INDIRECT_RADIANCE_ESTIMATION_PASS,
                0, 0);
			double time = sutilCurrentTime() - start;
			m_logger->log("3/7 Transfer photon map to GPU time: %1.3fs\n", time);
        }

        // Trace viewing rays
        {
			double start = sutilCurrentTime();
            nvtx::ScopedRange r("OptixEntryPoint::RAYTRACE_PASS");
            m_context->launch( OptixEntryPoint::PPM_RAYTRACE_PASS,
                static_cast<unsigned int>(m_width),
                static_cast<unsigned int>(m_height) );
			double time = sutilCurrentTime() - start;
			m_logger->log("4/7 RAYTRACE_PASS time: %1.3fs\n", time);
        }
    
        //
        // PPM Indirect Estimation (using the photon map)
        //

        {
			double start = sutilCurrentTime();
            nvtx::ScopedRange r("OptixEntryPoint::INDIRECT_RADIANCE_ESTIMATION");
            m_context->launch(OptixEntryPoint::PPM_INDIRECT_RADIANCE_ESTIMATION_PASS,
                m_width, m_height);
			double time = sutilCurrentTime() - start;
			m_logger->log("5/7 INDIRECT_RADIANCE_ESTIMATION time: %1.3fs\n", time);
        }

        //
        // Direct Radiance Estimation
        //

        {
			double start = sutilCurrentTime();
            nvtx::ScopedRange r("OptixEntryPoint::PPM_DIRECT_RADIANCE_ESTIMATION_PASS");
            m_context->launch(OptixEntryPoint::PPM_DIRECT_RADIANCE_ESTIMATION_PASS,
                m_width, m_height);
			double time = sutilCurrentTime() - start;
			m_logger->log("6/7 DIRECT_RADIANCE_ESTIMATION_PASS time: %1.3fs\n", time);
        }

        //
        // Combine indirect and direct buffers in the output buffer
        //
		{
			double start = sutilCurrentTime();
			nvtx::ScopedRange r("OptixEntryPoint::PPM_OUTPUT_PASS");
			m_context->launch(OptixEntryPoint::PPM_OUTPUT_PASS,
				m_width, m_height);
			double time = sutilCurrentTime() - start;
			m_logger->log("7/7 OUTPUT_PASS time: %1.3fs\n", time);
		}


        double traceTime = sutilCurrentTime() -traceStartTime;

		m_logger->log("END Trace time: %1.3fs\n", traceTime);

		// print scene meshes count
		int sceneNMeshes = m_context["sceneNMeshes"]->getInt();
		optix::Buffer hitsPerMeshBuffer = m_context["hitsPerMeshBuffer"]->getBuffer();
		unsigned int* bufferHost = (unsigned int*)hitsPerMeshBuffer->map();
		for(int i = 0; i < sceneNMeshes; i++)
		{
			if(bufferHost[i] > 0)
				m_logger->log("hitsPerMesh [%i] = %u\n", i, bufferHost[i]);
		}
		hitsPerMeshBuffer->unmap();
    }
    catch(const optix::Exception & e)
    {
        QString error = QString("An OptiX error occurred: %1").arg(e.getErrorString().c_str());
        throw std::exception(error.toLatin1().constData());
    }
}

static inline unsigned int max(unsigned int a, unsigned int b)
{
    return a > b ? a : b;
}

void PMOptixRenderer::resizeBuffers(unsigned int width, unsigned int height)
{
    m_outputBuffer->setSize( width, height );
    m_raytracePassOutputBuffer->setSize( width, height );
    m_outputBuffer->setSize( width, height );
    m_directRadianceBuffer->setSize( width, height );
    m_indirectRadianceBuffer->setSize( width, height );
    m_randomStatesBuffer->setSize(max(PHOTON_LAUNCH_WIDTH, (unsigned int)1280), max(PHOTON_LAUNCH_HEIGHT,  (unsigned int)768));
    initializeRandomStates();
    m_width = width;
    m_height = height;
}

unsigned int PMOptixRenderer::getWidth() const
{
    return m_width;
}

unsigned int PMOptixRenderer::getHeight() const
{
    return m_height;
}

void PMOptixRenderer::getOutputBuffer( void* data )
{
    void* buffer = reinterpret_cast<void*>( m_outputBuffer->map() );
    memcpy(data, buffer, getScreenBufferSizeBytes());
    m_outputBuffer->unmap();
}

unsigned int PMOptixRenderer::getScreenBufferSizeBytes() const
{
    return m_width*m_height*sizeof(optix::float3);
}

void PMOptixRenderer::debugOutputPhotonTracing()
{
}

void PMOptixRenderer::createGpuDebugBuffers()
{
}
