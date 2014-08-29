/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2008-2013 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/
#include <osgEarthUtil/Tessellator>

#include <osgEarthSymbology/Geometry>

using namespace osgEarth;
using namespace osgEarth::Util;


#define LC "[Tessellator] "

/***************************************************/

namespace
{

// Borrowed from osgUtil/DelaunayTriangulator.cpp
// Compute the circumcircle of a triangle (only x and y coordinates are used),
// return (Cx, Cy, r^2)
inline osg::Vec3 compute_circumcircle(
    const osg::Vec3 &a,
    const osg::Vec3 &b,
    const osg::Vec3 &c)
{
    float D =
        (a.x() - c.x()) * (b.y() - c.y()) -
        (b.x() - c.x()) * (a.y() - c.y());

    float cx, cy, r2;

    if(D==0.0)
    {
        // (Nearly) degenerate condition - either two of the points are equal (which we discount)
        // or the three points are colinear. In this case we just determine the average of
        // the three points as the centre for correctness, but squirt out a zero radius.
        // This method will produce a triangulation with zero area, so we have to check later
        cx = (a.x()+b.x()+c.x())/3.0;
        cy = (a.y()+b.y()+c.y())/3.0;
        r2 = 0.0;
    }
    else
    {
        cx =
        (((a.x() - c.x()) * (a.x() + c.x()) +
        (a.y() - c.y()) * (a.y() + c.y())) / 2 * (b.y() - c.y()) -
        ((b.x() - c.x()) * (b.x() + c.x()) +
        (b.y() - c.y()) * (b.y() + c.y())) / 2 * (a.y() - c.y())) / D;

        cy =
        (((b.x() - c.x()) * (b.x() + c.x()) +
        (b.y() - c.y()) * (b.y() + c.y())) / 2 * (a.x() - c.x()) -
        ((a.x() - c.x()) * (a.x() + c.x()) +
        (a.y() - c.y()) * (a.y() + c.y())) / 2 * (b.x() - c.x())) / D;

      //  r2 = (c.x() - cx) * (c.x() - cx) + (c.y() - cy) * (c.y() - cy);
        // the return r square is compared with r*r many times in an inner loop
        // so for efficiency use the inefficient sqrt once rather than 30* multiplies later.
        r2 = sqrt((c.x() - cx) * (c.x() - cx) + (c.y() - cy) * (c.y() - cy));
    }
    return osg::Vec3(cx, cy, r2);
}

// Test whether a point (only the x and y coordinates are used) lies inside
// a circle; the circle is passed as a vector: (Cx, Cy, r).

inline bool point_in_circle(const osg::Vec3 &point, const osg::Vec3 &circle)
{
    float r2 =
        (point.x() - circle.x()) * (point.x() - circle.x()) +
        (point.y() - circle.y()) * (point.y() - circle.y());
    return r2 <= circle.z()*circle.z();
//    return r2 <= circle.z();
}

struct TriIndices
{
    unsigned int a;
    unsigned int b;
    unsigned int c;

    TriIndices(unsigned int p1, unsigned int p2, unsigned int p3)
      : a(p1), b(p2), c(p3)
    {
      //nop
    }
};

typedef std::vector<TriIndices> TriList;

}

bool
Tessellator::tessellateGeometry(osg::Geometry &geom)
{
    //TODO: Currently just assuming there is a single PrimitiveSet and that
    //      all verts in the vertext array are part of the outer ring of the
    //      polygon.  Need to iterate through the PrimitiveSets and handle
    //      them appropriately...tessellate individually if they are separate
    //      primitives, or use the coincident edge method to combine them
    //      with the outer ring if they are holes.

    osg::Vec3Array* vertices = dynamic_cast<osg::Vec3Array*>(geom.getVertexArray());

    if (!vertices || vertices->empty() || geom.getPrimitiveSetList().empty()) return false;

    // we currently don't handle geometry which use indices...
    if (geom.getVertexIndices() ||
        geom.getNormalIndices() ||
        geom.getColorIndices() ||
        geom.getSecondaryColorIndices() ||
        geom.getFogCoordIndices()) return false;

    // not even text coord indices don't handle geometry which use indices...
    for(unsigned int unit=0;unit<geom.getNumTexCoordArrays();++unit)
    {
        if (geom.getTexCoordIndices(unit)) return false;
    }

    std::vector<unsigned int> activeVerts;
    for (unsigned int i=0; i < vertices->size(); i++)
    {
        activeVerts.push_back(i);
    }

    TriList tris;

    bool success = true;
    unsigned int cursor = 0;
    unsigned int cursor_start = 0;
    unsigned int tradCursor = UINT_MAX;
    while (activeVerts.size() > 3)
    {
        if (isConvex(*vertices, activeVerts, cursor))
        {
            bool tradEar = tradCursor != UINT_MAX;
            if (isEar(*vertices, activeVerts, cursor, tradEar))
            {
                unsigned int prev = cursor == 0 ? activeVerts.size() - 1 : cursor - 1;
                unsigned int next = cursor == activeVerts.size() - 1 ? 0 : cursor + 1;

                tris.push_back(TriIndices(activeVerts[prev], activeVerts[cursor], activeVerts[next]));
              
                activeVerts.erase(activeVerts.begin() + cursor);
                if (cursor >= activeVerts.size())
                    cursor = 0;

                cursor_start = cursor;
                tradCursor = UINT_MAX;

                continue;
            }
            
            if (tradEar && tradCursor == UINT_MAX)
            {
                tradCursor = cursor;
            }
        }

        cursor++;
        if (cursor >= activeVerts.size())
            cursor = 0;

        if (cursor == cursor_start)
        {
            if (tradCursor != UINT_MAX)
            {
                // No ear was found with circumcircle test, use first traditional ear found

                //TODO: could improve by tracking and using the traditional ear with that is
                //      the least skinny instead of just the first found

                cursor = tradCursor;

                unsigned int prev = cursor == 0 ? activeVerts.size() - 1 : cursor - 1;
                unsigned int next = cursor == activeVerts.size() - 1 ? 0 : cursor + 1;

                tris.push_back(TriIndices(activeVerts[prev], activeVerts[cursor], activeVerts[next]));
                  
                activeVerts.erase(activeVerts.begin() + cursor);
                if (cursor >= activeVerts.size())
                    cursor = 0;

                cursor_start = cursor;
                tradCursor = UINT_MAX;

                continue;
            }
            else
            {
                success = false;
                break;
            }
        }
    }

    if (success)
    {
        if (activeVerts.size() == 3)
        {
            // add last tri
            tris.push_back(TriIndices(activeVerts[0], activeVerts[1], activeVerts[2]));
        }

        // remove the existing primitives.
        unsigned int nprimsetoriginal= geom.getNumPrimitiveSets();
        if (nprimsetoriginal) geom.removePrimitiveSet(0, nprimsetoriginal);

        osg::DrawElementsUInt* triElements = new osg::DrawElementsUInt(osg::PrimitiveSet::TRIANGLES, 0);
        for (TriList::const_iterator it = tris.begin(); it != tris.end(); ++it)
        {
            triElements->push_back(it->a);
            triElements->push_back(it->b);
            triElements->push_back(it->c);
        }

        geom.addPrimitiveSet(triElements);
    }
    else
    {
        //TODO: handle
        OE_WARN << LC << "Tessellation failed!" << std::endl;
    }

    return success;
}

bool
Tessellator::isConvex(const osg::Vec3Array &vertices, const std::vector<unsigned int> &activeVerts, unsigned int cursor)
{
    unsigned int prev = cursor == 0 ? activeVerts.size() - 1 : cursor - 1;
    unsigned int next = cursor == activeVerts.size() - 1 ? 0 : cursor + 1;

    unsigned int a = activeVerts[prev];
    unsigned int b = activeVerts[cursor];
    unsigned int c = activeVerts[next];

    osg::Vec3d dataA;
    dataA._v[0] = vertices[a][0];
    dataA._v[1] = vertices[a][1];
    dataA._v[2] = vertices[a][2];

    osg::Vec3d dataB;
    dataB._v[0] = vertices[b][0];
    dataB._v[1] = vertices[b][1];
    dataB._v[2] = vertices[b][2];

    osg::Vec3d dataC;
    dataC._v[0] = vertices[c][0];
    dataC._v[1] = vertices[c][1];
    dataC._v[2] = vertices[c][2];

    //http://www.gamedev.net/topic/542870-determine-which-side-of-a-line-a-point-is/#entry4500667
    //(Bx - Ax) * (Cy - Ay) - (By - Ay) * (Cx - Ax)

    return (dataB.x() - dataA.x()) * (dataC.y() - dataA.y()) - (dataB.y() - dataA.y()) * (dataC.x() - dataA.x()) > 0.0;
}

bool
Tessellator::isEar(const osg::Vec3Array &vertices, const std::vector<unsigned int> &activeVerts, unsigned int cursor, bool &tradEar)
{
    unsigned int prev = cursor == 0 ? activeVerts.size() - 1 : cursor - 1;
    unsigned int next = cursor == activeVerts.size() - 1 ? 0 : cursor + 1;

    osg::Vec3d cc(compute_circumcircle(vertices[activeVerts[prev]], vertices[activeVerts[cursor]], vertices[activeVerts[next]]));

    osgEarth::Symbology::Polygon poly(3);
    poly.push_back(vertices[activeVerts[prev]]);
    poly.push_back(vertices[activeVerts[cursor]]);
    poly.push_back(vertices[activeVerts[next]]);

    unsigned int nextNext = next == activeVerts.size() - 1 ? 0 : next + 1;

		// Check every point not part of the ear
    bool circEar = true;
		while( nextNext != prev )
		{
        unsigned int p = activeVerts[nextNext];

        if (circEar && point_in_circle(vertices[p], cc))
        {
            circEar = false;

            if (tradEar)
              return false;
        }

        if (!tradEar && poly.contains2D(vertices[p].x(), vertices[p].y()))
			  {
            return false;
			  }

			  nextNext = nextNext == activeVerts.size() - 1 ? 0 : nextNext + 1;
		}

    tradEar = true;

		return circEar;
}