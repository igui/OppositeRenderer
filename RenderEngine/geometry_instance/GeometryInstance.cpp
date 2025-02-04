/* 
 * Copyright (c) 2013 Opposite Renderer
 * For the full copyright and license information, please view the LICENSE.txt
 * file that was distributed with this source code.
*/

#include "GeometryInstance.h"
#include "material/Material.h"
optix::GeometryInstance GeometryInstance::getOptixGeometryInstance( optix::Context & context, bool useHoleCheckProgram)
{
    optix::Geometry sphereGeometry = this->getOptixGeometry(context);
	optix::Material omaterial = this->m_material.getOptixMaterial(context, useHoleCheckProgram);
    optix::GeometryInstance gi = context->createGeometryInstance( sphereGeometry, &omaterial, &omaterial+1 );
	this->m_material.registerInstanceValues(gi);
    return gi;
}
