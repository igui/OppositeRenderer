/* 
 * Copyright (c) 2014 Opposite Renderer
 * For the full copyright and license information, please view the LICENSE.txt
 * file that was distributed with this source code.
 */

#pragma once
#include "Material.h"
#include "math/Vector3.h"

class Hole : public Material
{
private:
    static bool m_optixMaterialIsCreated;
    static optix::Material m_optixMaterial;
public:
    Hole();
    virtual optix::Material getOptixMaterial(optix::Context & context, bool useHoleCheckProgram);
    virtual void registerGeometryInstanceValues(optix::GeometryInstance & instance);
	virtual Material* clone();
};