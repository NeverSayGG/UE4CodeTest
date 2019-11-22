// ======================================================================== //
// Copyright 2009-2015 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#pragma once

#include "triangle1.h"
#include "../../common/ray.h"
#include "filter.h"

namespace embree
{
  /*! Intersector for individual precomputed triangles with one
   *  ray. This intersector implements a modified version of the
   *  Moeller Trumbore intersector from the paper "Fast, Minimum
   *  Storage Ray-Triangle Intersection". In contrast to the paper we
   *  precalculate some factors and factor the calculations
   *  differently to allow precalculating the cross product e1 x
   *  e2. */

  template< bool ENABLE_INTERSECTION_FILTER>
    struct Triangle1Intersector1MoellerTrumbore
    {

      __forceinline static bool intersect1(const float16 &dir_xyz,
					   const float16 &org_xyz,
					   const float16 &min_dist_xyz,
					   float16 &max_dist_xyz,
					   const int16 &and_mask,
					   Ray& ray, 
					   const Scene     *__restrict__ const geometry,
					   const Triangle1 * __restrict__ const tptr)
      {
	const float16 zero = float16::zero();
	prefetch<PFHINT_L1>(tptr + 3);
	prefetch<PFHINT_L1>(tptr + 2);
	prefetch<PFHINT_L1>(tptr + 1);
	prefetch<PFHINT_L1>(tptr + 0); 
	      
	const float16 v0 = gather_4f_zlc(and_mask,
				       (float*)&tptr[0].v0,
				       (float*)&tptr[1].v0,
				       (float*)&tptr[2].v0,
				       (float*)&tptr[3].v0);
	      
	const float16 v1 = gather_4f_zlc(and_mask,
				       (float*)&tptr[0].v1,
				       (float*)&tptr[1].v1,
				       (float*)&tptr[2].v1,
				       (float*)&tptr[3].v1);
	      
	const float16 v2 = gather_4f_zlc(and_mask,
				       (float*)&tptr[0].v2,
				       (float*)&tptr[1].v2,
				       (float*)&tptr[2].v2,
				       (float*)&tptr[3].v2);

	const float16 e1 = v1 - v0;
	const float16 e2 = v0 - v2;	     
	const float16 normal = lcross_zxy(e1,e2);
	const float16 org = v0 - org_xyz;
	const float16 odzxy = msubr231(org * swizzle(dir_xyz,_MM_SWIZ_REG_DACB), dir_xyz, swizzle(org,_MM_SWIZ_REG_DACB));
	const float16 den = ldot3_zxy(dir_xyz,normal);	      
	const float16 rcp_den = rcp(den);
	const float16 uu = ldot3_zxy(e2,odzxy); 
	const float16 vv = ldot3_zxy(e1,odzxy); 
	const float16 u = uu * rcp_den;
	const float16 v = vv * rcp_den;

#if defined(RTCORE_BACKFACE_CULLING)
	const bool16 m_init = (bool16)0x1111 & (den > zero);
#else
	const bool16 m_init = 0x1111;
#endif

	const bool16 valid_u = ge(m_init,u,zero);
	const bool16 valid_v = ge(valid_u,v,zero);
	const bool16 m_aperture = le(valid_v,u+v,float16::one()); 

	const float16 nom = ldot3_zxy(org,normal);

	if (unlikely(none(m_aperture))) return false;
	const float16 t = rcp_den*nom;

	bool16 m_final  = lt(lt(m_aperture,min_dist_xyz,t),t,max_dist_xyz);

#if defined(RTCORE_RAY_MASK)
	const int16 rayMask(ray.mask);
	const int16 triMask = getTriMasks(tptr);
	const bool16 m_ray_mask = (rayMask & triMask) != int16::zero();
	m_final &= m_ray_mask;	      
#endif


	//////////////////////////////////////////////////////////////////////////////////////////////////

	/* did the ray hot one of the four triangles? */
	if (unlikely(any(m_final)))
	  {
	    /* intersection filter test */
	    if (ENABLE_INTERSECTION_FILTER) 
	      {
		float16 org_max_dist_xyz = max_dist_xyz;

		/* did the ray hit one of the four triangles? */
		while (any(m_final)) 
		  {
		    max_dist_xyz  = select(m_final,t,org_max_dist_xyz);
		    const float16 min_dist = vreduce_min(max_dist_xyz);
		    const bool16 m_dist = eq(min_dist,max_dist_xyz);
		    const size_t vecIndex = bitscan(toInt(m_dist));
		    const size_t triIndex = vecIndex >> 2;
		    const Triangle1  *__restrict__ tri_ptr = tptr + triIndex;
		    const bool16 m_tri = m_dist^(m_dist & (bool16)((unsigned int)m_dist - 1));
		    const float16 gnormalx = float16(tri_ptr->Ng.x);
		    const float16 gnormaly = float16(tri_ptr->Ng.y);
		    const float16 gnormalz = float16(tri_ptr->Ng.z);
		    const int geomID = tri_ptr->geomID();
		    const int primID = tri_ptr->primID();
                
		    const Geometry* geom = geometry->get(geomID);
		    if (likely(!geom->hasIntersectionFilter1())) 
		      {
			ray.update(m_tri,min_dist,u,v,gnormalx,gnormaly,gnormalz,geomID,primID);
			break;
		      }
                
		    if (runIntersectionFilter1(geom,ray,u,v,min_dist,gnormalx,gnormaly,gnormalz,m_tri,geomID,primID)) {
		      break;
		    }
		    m_final ^= m_tri;
		  }
		max_dist_xyz = ray.tfar;
	      }
	    else
	      {
		max_dist_xyz  = select(m_final,t,max_dist_xyz);
		const float16 min_dist = vreduce_min(max_dist_xyz);
		const bool16 m_dist = eq(min_dist,max_dist_xyz);

		prefetch<PFHINT_L1EX>((float16*)&ray + 0);
		prefetch<PFHINT_L1EX>((float16*)&ray + 1);

		const size_t vecIndex = bitscan(toInt(m_dist));
		const size_t triIndex = vecIndex >> 2;

		const Triangle1  *__restrict__ tri_ptr = tptr + triIndex;

		const bool16 m_tri = m_dist^(m_dist & (bool16)((unsigned int)m_dist - 1));

		const float16 gnormalx = float16(tri_ptr->Ng.x);
		const float16 gnormaly = float16(tri_ptr->Ng.y);
		const float16 gnormalz = float16(tri_ptr->Ng.z);

		max_dist_xyz = min_dist;

		ray.update(m_tri,min_dist,u,v,gnormalx,gnormaly,gnormalz,tri_ptr->geomID(),tri_ptr->primID());
	      }
	  return true;

	}
      return false;
    }


    __forceinline static bool16 occluded1(const float16 &dir_xyz,
					 const float16 &org_xyz,
					 const float16 &min_dist_xyz,
					 const float16 &max_dist_xyz,
					 const int16 &and_mask,
					 const Ray& ray, 
					 const Scene     *__restrict__ const geometry,
					 const Triangle1 * __restrict__ const tptr)
    {
      const float16 zero = float16::zero();

      prefetch<PFHINT_L1>(tptr + 3);
      prefetch<PFHINT_L1>(tptr + 2);
      prefetch<PFHINT_L1>(tptr + 1);
      prefetch<PFHINT_L1>(tptr + 0); 

	      
      const float16 v0 = gather_4f_zlc(and_mask,
				     (float*)&tptr[0].v0,
				     (float*)&tptr[1].v0,
				     (float*)&tptr[2].v0,
				     (float*)&tptr[3].v0);
	      
      const float16 v1 = gather_4f_zlc(and_mask,
				     (float*)&tptr[0].v1,
				     (float*)&tptr[1].v1,
				     (float*)&tptr[2].v1,
				     (float*)&tptr[3].v1);
	      
      const float16 v2 = gather_4f_zlc(and_mask,
				     (float*)&tptr[0].v2,
				     (float*)&tptr[1].v2,
				     (float*)&tptr[2].v2,
				     (float*)&tptr[3].v2);

      const float16 e1 = v1 - v0;
      const float16 e2 = v0 - v2;	     
      const float16 normal = lcross_zxy(e1,e2);
      const float16 org = v0 - org_xyz;
      const float16 odzxy = msubr231(org * swizzle(dir_xyz,_MM_SWIZ_REG_DACB), dir_xyz, swizzle(org,_MM_SWIZ_REG_DACB));
      const float16 den = ldot3_zxy(dir_xyz,normal);	      
      const float16 rcp_den = rcp(den);
      const float16 uu = ldot3_zxy(e2,odzxy); 
      const float16 vv = ldot3_zxy(e1,odzxy); 
      const float16 u = uu * rcp_den;
      const float16 v = vv * rcp_den;

#if defined(RTCORE_BACKFACE_CULLING)
      const bool16 m_init = (bool16)0x1111 & (den > zero);
#else
      const bool16 m_init = 0x1111;
#endif

      const bool16 valid_u = ge((bool16)m_init,u,zero);
      const bool16 valid_v = ge(valid_u,v,zero);
      const bool16 m_aperture = le(valid_v,u+v,float16::one()); 

      const float16 nom = ldot3_zxy(org,normal);
      const float16 t = rcp_den*nom;
      if (unlikely(none(m_aperture))) return m_aperture;

      bool16 m_final  = lt(lt(m_aperture,min_dist_xyz,t),t,max_dist_xyz);

#if defined(RTCORE_RAY_MASK)
      const int16 rayMask(ray.mask);
      const int16 triMask = getTriMasks(tptr); 
      const bool16 m_ray_mask = (rayMask & triMask) != int16::zero();
      m_final &= m_ray_mask;	      
#endif

      if (ENABLE_INTERSECTION_FILTER)
	{
	  /* did the ray hit one of the four triangles? */
	  while (any(m_final)) 
	    {
	      const float16 temp_t  = select(m_final,t,max_dist_xyz);
	      const float16 min_dist = vreduce_min(temp_t);
	      const bool16 m_dist = eq(min_dist,temp_t);
	      const size_t vecIndex = bitscan(toInt(m_dist));
	      const size_t triIndex = vecIndex >> 2;
	      const Triangle1  *__restrict__ tri_ptr = tptr + triIndex;
	      const bool16 m_tri = m_dist^(m_dist & (bool16)((unsigned int)m_dist - 1));
	      const float16 gnormalx = float16(tri_ptr->Ng.x);
	      const float16 gnormaly = float16(tri_ptr->Ng.y);
	      const float16 gnormalz = float16(tri_ptr->Ng.z);
	      const int geomID = tri_ptr->geomID();
	      const int primID = tri_ptr->primID();                
	      const Geometry* geom = geometry->get(geomID);

	      if (likely(!geom->hasOcclusionFilter1())) break;
                
	      if (runOcclusionFilter1(geom,(Ray&)ray,u,v,min_dist,gnormalx,gnormaly,gnormalz,m_tri,geomID,primID)) 
		break;

	      m_final ^= m_tri; /* clear bit */
	    }
	}

      return m_final;
    }

  };


  //////////////////////////////////////////////////////////////////////////////////////////////////
  //////////////////////////////////////////////////////////////////////////////////////////////////
  //////////////////////////////////////////////////////////////////////////////////////////////////

  template< bool ENABLE_INTERSECTION_FILTER>
    struct Triangle1Intersector1MoellerTrumboreRobust
    {

#if 1

      __forceinline static bool intersect1(const float16 &dir_xyz,
					   const float16 &org_xyz,
					   const float16 &min_dist_xyz,
					   float16 &max_dist_xyz,
					   const int16 &and_mask,
					   Ray& ray, 
					   const Precalculations &pre,
					   const Scene     *__restrict__ const geometry,
					   const Triangle1 * __restrict__ const tptr)
      {
	const float16 zero = float16::zero();
	prefetch<PFHINT_L1>(tptr + 3);
	prefetch<PFHINT_L1>(tptr + 2);
	prefetch<PFHINT_L1>(tptr + 1);
	prefetch<PFHINT_L1>(tptr + 0); 
	const float16 _v0 = gather_4f_zlc(and_mask,
					(float*)&tptr[0].v0,
					(float*)&tptr[1].v0,
					(float*)&tptr[2].v0,
					(float*)&tptr[3].v0);
	      
	const float16 _v1 = gather_4f_zlc(and_mask,
					(float*)&tptr[0].v1,
					(float*)&tptr[1].v1,
					(float*)&tptr[2].v1,
					(float*)&tptr[3].v1);
	      
	const float16 _v2 = gather_4f_zlc(and_mask,
					(float*)&tptr[0].v2,
					(float*)&tptr[1].v2,
					(float*)&tptr[2].v2,
					(float*)&tptr[3].v2);

	const float16 v0 = _v0 * pre.rdir_xyz - pre.org_rdir_xyz;
	const float16 v1 = _v1 * pre.rdir_xyz - pre.org_rdir_xyz;
	const float16 v2 = _v2 * pre.rdir_xyz - pre.org_rdir_xyz;

	const float16 e0 = v2 - v0;
	const float16 e1 = v0 - v1;	     
	const float16 e2 = v1 - v2;	  
	

	const float16 Ng1     = lcross_xyz(e1,e0);
	float16 Ng      = Ng1+Ng1;
	const float16 den     = lsum3_xyz(Ng);	      
	const float16 rcp_den = rcp(den);

#if defined(RTCORE_BACKFACE_CULLING)
	bool16 m_valid = (bool16)0x1111 & (den > zero);
#else
	bool16 m_valid = (bool16)0x1111;
#endif

	const float16 u = lsum3_xyz(lcross_xyz(v2+v0,e0)) * rcp_den; 
	m_valid       = ge( m_valid, u, zero);

	const float16 v = lsum3_xyz(lcross_xyz(v0+v1,e1)) * rcp_den; 
	m_valid       = ge( m_valid, v, zero);

	const float16 w = lsum3_xyz(lcross_xyz(v1+v2,e2)) * rcp_den;  
	m_valid       = ge( m_valid, w, zero);


	if (unlikely(none(m_valid))) return false;

	const bool16 m_den = ne(m_valid,den,zero);
	const float16 t = ldot3_xyz(v0,Ng) * rcp_den;
	bool16 m_final      = lt(lt(m_den,min_dist_xyz,t),t,max_dist_xyz);

#if defined(RTCORE_RAY_MASK)
	const int16 rayMask(ray.mask);
	const int16 triMask = getTriMasks(tptr);
	const bool16 m_ray_mask = (rayMask & triMask) != int16::zero();
	m_final &= m_ray_mask;	      
#endif

	/* correct normal */
	Ng *= swizzle(dir_xyz * swizzle(dir_xyz,_MM_SWIZ_REG_DACB),_MM_SWIZ_REG_DACB);

	//////////////////////////////////////////////////////////////////////////////////////////////////

	/* did the ray hot one of the four triangles? */
	if (unlikely(any(m_final)))
	  {
	    /* intersection filter test */
	    if (ENABLE_INTERSECTION_FILTER) 
	      {
		float16 org_max_dist_xyz = max_dist_xyz;

		/* did the ray hit one of the four triangles? */
		while (any(m_final)) 
		  {
		    max_dist_xyz  = select(m_final,t,org_max_dist_xyz);
		    const float16 min_dist = vreduce_min(max_dist_xyz);
		    const bool16 m_dist = eq(min_dist,max_dist_xyz);
		    const size_t vecIndex = bitscan(toInt(m_dist));
		    const size_t triIndex = vecIndex >> 2;
		    const Triangle1  *__restrict__ tri_ptr = tptr + triIndex;
		    const bool16 m_tri = m_dist^(m_dist & (bool16)((unsigned int)m_dist - 1));
		    const float16 gnormalx = float16(tri_ptr->Ng.x);
		    const float16 gnormaly = float16(tri_ptr->Ng.y);
		    const float16 gnormalz = float16(tri_ptr->Ng.z);
		    const int geomID = tri_ptr->geomID();
		    const int primID = tri_ptr->primID();
                
		    const Geometry* geom = geometry->get(geomID);
		    if (likely(!geom->hasIntersectionFilter1())) 
		      {
			ray.update(m_tri,min_dist,u,v,gnormalx,gnormaly,gnormalz,geomID,primID);
			break;
		      }
                
		    if (runIntersectionFilter1(geom,ray,u,v,min_dist,gnormalx,gnormaly,gnormalz,m_tri,geomID,primID)) {
		      break;
		    }
		    m_final ^= m_tri;
		  }
		max_dist_xyz = ray.tfar;
	      }
	    else
	      {
		max_dist_xyz  = select(m_final,t,max_dist_xyz);
		const float16 min_dist = vreduce_min(max_dist_xyz);
		const bool16 m_dist = eq(min_dist,max_dist_xyz);

		prefetch<PFHINT_L1EX>((float16*)&ray + 0);
		prefetch<PFHINT_L1EX>((float16*)&ray + 1);

		const size_t vecIndex = bitscan(toInt(m_dist));
		const size_t triIndex = vecIndex >> 2;

		const Triangle1  *__restrict__ tri_ptr = tptr + triIndex;

		const bool16 m_tri = m_dist^(m_dist & (bool16)((unsigned int)m_dist - 1));

		const float16 gnormalx = float16(tri_ptr->Ng.x);
		const float16 gnormaly = float16(tri_ptr->Ng.y);
		const float16 gnormalz = float16(tri_ptr->Ng.z);

		max_dist_xyz = min_dist;

		ray.update(m_tri,min_dist,u,v,gnormalx,gnormaly,gnormalz,tri_ptr->geomID(),tri_ptr->primID());
	      }
	  return true;

	}
      return false;
    }

#else
      __forceinline static bool intersect1(const float16 &dir_xyz,
					   const float16 &org_xyz,
					   const float16 &min_dist_xyz,
					   float16 &max_dist_xyz,
					   const int16 &and_mask,
					   Ray& ray, 
					   const Precalculations &pre,
					   const Scene     *__restrict__ const geometry,
					   const Triangle1 * __restrict__ const tptr)
      {
	const float16 zero = float16::zero();
	prefetch<PFHINT_L1>(tptr + 3);
	prefetch<PFHINT_L1>(tptr + 2);
	prefetch<PFHINT_L1>(tptr + 1);
	prefetch<PFHINT_L1>(tptr + 0); 
	const float16 _v0 = gather_4f_zlc(and_mask,
					(float*)&tptr[0].v0,
					(float*)&tptr[1].v0,
					(float*)&tptr[2].v0,
					(float*)&tptr[3].v0);
	      
	const float16 _v1 = gather_4f_zlc(and_mask,
					(float*)&tptr[0].v1,
					(float*)&tptr[1].v1,
					(float*)&tptr[2].v1,
					(float*)&tptr[3].v1);
	      
	const float16 _v2 = gather_4f_zlc(and_mask,
					(float*)&tptr[0].v2,
					(float*)&tptr[1].v2,
					(float*)&tptr[2].v2,
					(float*)&tptr[3].v2);
	const float16 v0 = _v0 - org_xyz;
	const float16 v1 = _v1 - org_xyz;
	const float16 v2 = _v2 - org_xyz;

	const float16 e0 = v2 - v0;
	const float16 e1 = v0 - v1;	     
	const float16 e2 = v1 - v2;	  
	

	const float16 Ng1     = lcross_xyz(e1,e0);
	const float16 Ng      = Ng1+Ng1;
	const float16 den     = ldot3_xyz(Ng,dir_xyz);	      
	const float16 rcp_den = rcp(den);

#if defined(RTCORE_BACKFACE_CULLING)
	bool16 m_valid = (bool16)0x1111 & (den > zero);
#else
	bool16 m_valid = (bool16)0x1111;
#endif

	const float16 u = ldot3_xyz(lcross_xyz(v2+v0,e0),dir_xyz) * rcp_den; 
	m_valid       = ge( m_valid, u, zero);

	const float16 v = ldot3_xyz(lcross_xyz(v0+v1,e1),dir_xyz) * rcp_den; 
	m_valid       = ge( m_valid, v, zero);

	const float16 w = ldot3_xyz(lcross_xyz(v1+v2,e2),dir_xyz) * rcp_den;  
	m_valid       = ge( m_valid, w, zero);


	if (unlikely(none(m_valid))) return false;

	const bool16 m_den = ne(m_valid,den,zero);
	const float16 t = ldot3_xyz(v0,Ng) * rcp_den;
	bool16 m_final      = lt(lt(m_den,min_dist_xyz,t),t,max_dist_xyz);

#if defined(RTCORE_RAY_MASK)
	const int16 rayMask(ray.mask);
	const int16 triMask = getTriMasks(tptr);
	const bool16 m_ray_mask = (rayMask & triMask) != int16::zero();
	m_final &= m_ray_mask;	      
#endif


	//////////////////////////////////////////////////////////////////////////////////////////////////

	/* did the ray hot one of the four triangles? */
	if (unlikely(any(m_final)))
	  {
	    /* intersection filter test */
	    if (ENABLE_INTERSECTION_FILTER) 
	      {
		float16 org_max_dist_xyz = max_dist_xyz;

		/* did the ray hit one of the four triangles? */
		while (any(m_final)) 
		  {
		    max_dist_xyz  = select(m_final,t,org_max_dist_xyz);
		    const float16 min_dist = vreduce_min(max_dist_xyz);
		    const bool16 m_dist = eq(min_dist,max_dist_xyz);
		    const size_t vecIndex = bitscan(toInt(m_dist));
		    const size_t triIndex = vecIndex >> 2;
		    const Triangle1  *__restrict__ tri_ptr = tptr + triIndex;
		    const bool16 m_tri = m_dist^(m_dist & (bool16)((unsigned int)m_dist - 1));
		    const float16 gnormalx = float16(tri_ptr->Ng.x);
		    const float16 gnormaly = float16(tri_ptr->Ng.y);
		    const float16 gnormalz = float16(tri_ptr->Ng.z);
		    const int geomID = tri_ptr->geomID();
		    const int primID = tri_ptr->primID();
                
		    const Geometry* geom = geometry->get(geomID);
		    if (likely(!geom->hasIntersectionFilter1())) 
		      {
			ray.update(m_tri,min_dist,u,v,gnormalx,gnormaly,gnormalz,geomID,primID);
			break;
		      }
                
		    if (runIntersectionFilter1(geom,ray,u,v,min_dist,gnormalx,gnormaly,gnormalz,m_tri,geomID,primID)) {
		      break;
		    }
		    m_final ^= m_tri;
		  }
		max_dist_xyz = ray.tfar;
	      }
	    else
	      {
		max_dist_xyz  = select(m_final,t,max_dist_xyz);
		const float16 min_dist = vreduce_min(max_dist_xyz);
		const bool16 m_dist = eq(min_dist,max_dist_xyz);

		prefetch<PFHINT_L1EX>((float16*)&ray + 0);
		prefetch<PFHINT_L1EX>((float16*)&ray + 1);

		const size_t vecIndex = bitscan(toInt(m_dist));
		const size_t triIndex = vecIndex >> 2;

		const Triangle1  *__restrict__ tri_ptr = tptr + triIndex;

		const bool16 m_tri = m_dist^(m_dist & (bool16)((unsigned int)m_dist - 1));

		const float16 gnormalx = float16(tri_ptr->Ng.x);
		const float16 gnormaly = float16(tri_ptr->Ng.y);
		const float16 gnormalz = float16(tri_ptr->Ng.z);

		max_dist_xyz = min_dist;

		ray.update(m_tri,min_dist,u,v,gnormalx,gnormaly,gnormalz,tri_ptr->geomID(),tri_ptr->primID());
	      }
	  return true;

	}
      return false;
    }
#endif

      __forceinline static bool16 occluded1(const float16 &dir_xyz,
					   const float16 &org_xyz,
					   const float16 &min_dist_xyz,
					   const float16 &max_dist_xyz,
					   const int16 &and_mask,
					   const Ray& ray, 
					   const Precalculations &pre,
					   const Scene     *__restrict__ const geometry,
					   const Triangle1 * __restrict__ const tptr)
      {
	const float16 zero = float16::zero();

	prefetch<PFHINT_L1>(tptr + 3);
	prefetch<PFHINT_L1>(tptr + 2);
	prefetch<PFHINT_L1>(tptr + 1);
	prefetch<PFHINT_L1>(tptr + 0); 
	const float16 _v0 = gather_4f_zlc(and_mask,
					(float*)&tptr[0].v0,
					(float*)&tptr[1].v0,
					(float*)&tptr[2].v0,
					(float*)&tptr[3].v0);
	      
	const float16 _v1 = gather_4f_zlc(and_mask,
					(float*)&tptr[0].v1,
					(float*)&tptr[1].v1,
					(float*)&tptr[2].v1,
					(float*)&tptr[3].v1);
	      
	const float16 _v2 = gather_4f_zlc(and_mask,
					(float*)&tptr[0].v2,
					(float*)&tptr[1].v2,
					(float*)&tptr[2].v2,
					(float*)&tptr[3].v2);

	      
	const float16 v0 = _v0 - org_xyz;
	const float16 v1 = _v1 - org_xyz;
	const float16 v2 = _v2 - org_xyz;

	const float16 e0 = v2 - v0;
	const float16 e1 = v0 - v1;	     
	const float16 e2 = v1 - v2;	     

	const float16 Ng1     = lcross_xyz(e1,e0);
	const float16 Ng      = Ng1+Ng1;
	const float16 den     = ldot3_xyz(Ng,dir_xyz);	      
	const float16 rcp_den = rcp(den);

#if defined(RTCORE_BACKFACE_CULLING)
	bool16 m_valid = (bool16)0x1111 & (den > zero);
#else
	bool16 m_valid = (bool16)0x1111;
#endif

	const float16 u = ldot3_xyz(lcross_xyz(v2+v0,e0),dir_xyz) * rcp_den; 
	m_valid       = ge( m_valid, u, zero);

	const float16 v = ldot3_xyz(lcross_xyz(v0+v1,e1),dir_xyz) * rcp_den; 
	m_valid       = ge( m_valid, v, zero);

	const float16 w = ldot3_xyz(lcross_xyz(v1+v2,e2),dir_xyz) * rcp_den;  
	m_valid       = ge( m_valid, w, zero);

	const bool16 m_den = ne(m_valid,den,zero);
	const float16 t = ldot3_xyz(v0,Ng) * rcp_den;

	if (unlikely(none(m_valid))) return false;

	bool16 m_final  = lt(lt(m_valid,min_dist_xyz,t),t,max_dist_xyz);

#if defined(RTCORE_RAY_MASK) // FIXME: this is broken
	//const int16 rayMask(ray16.mask[rayIndex]);
	//const int16 triMask = getTriMasks(tptr); 
	//const bool16 m_ray_mask = (rayMask & triMask) != int16::zero();
	//m_final &= m_ray_mask;	      
#endif

	if (ENABLE_INTERSECTION_FILTER)
	  {
	    /* did the ray hit one of the four triangles? */
	    while (any(m_final)) 
	      {
		const float16 temp_t  = select(m_final,t,max_dist_xyz);
		const float16 min_dist = vreduce_min(temp_t);
		const bool16 m_dist = eq(min_dist,temp_t);
		const size_t vecIndex = bitscan(toInt(m_dist));
		const size_t triIndex = vecIndex >> 2;
		const Triangle1  *__restrict__ tri_ptr = tptr + triIndex;
		const bool16 m_tri = m_dist^(m_dist & (bool16)((unsigned int)m_dist - 1));
		const float16 gnormalx = float16(tri_ptr->Ng.x);
		const float16 gnormaly = float16(tri_ptr->Ng.y);
		const float16 gnormalz = float16(tri_ptr->Ng.z);
		const int geomID = tri_ptr->geomID();
		const int primID = tri_ptr->primID();                
		const Geometry* geom = geometry->get(geomID);

		if (likely(!geom->hasOcclusionFilter1())) break;
                
		if (runOcclusionFilter1(geom,(Ray&)ray,u,v,min_dist,gnormalx,gnormaly,gnormalz,m_tri,geomID,primID)) 
		  break;

		m_final ^= m_tri; /* clear bit */
	      }
	  }

	return m_final;
      }

    };

}
