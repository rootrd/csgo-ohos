#include "mathlib/volumeculler.h"
#include <cstdio>
#include <cstdlib>

static void Require( bool condition, const char *message )
{
	if ( !condition )
	{
		std::fprintf( stderr, "FAIL: %s\n", message );
		std::exit( 1 );
	}
}

static void CheckBox( const CVolumeCuller &culler, const Vector &center,
	const Vector &extent, bool expected )
{
	VectorAligned alignedMin( center - extent ), alignedMax( center + extent );
	VectorAligned alignedCenter( center ), alignedExtent( extent );
	// The Vector overload uses a four-lane load, so provide its padding too.
	struct { Vector value; float padding; } mins = { center - extent, 0 }, maxs = { center + extent, 0 };
	Require( culler.CheckBox( alignedMin, alignedMax ) == expected, "aligned AABB clipping" );
	Require( culler.CheckBox( mins.value, maxs.value ) == expected, "Vector AABB clipping" );
	Require( culler.CheckBoxCenterHalfDiagonal( alignedCenter, alignedExtent ) == expected,
		"center/extent clipping" );
}

int main()
{
	CVolumeCuller culler;
	Require( !culler.IsValid(), "empty culler must be inactive" );
	CheckBox( culler, Vector( 50, 50, 50 ), Vector( 1, 1, 1 ), true );

	// A base frustum remains useful if the optional caster inclusion volume
	// cannot be constructed. The view builder must not discard this culler.
	VPlane planes[6];
	planes[0].Init( Vector( 1, 0, 0 ), -1 );
	planes[1].Init( Vector( -1, 0, 0 ), -1 );
	planes[2].Init( Vector( 0, 1, 0 ), -1 );
	planes[3].Init( Vector( 0, -1, 0 ), -1 );
	planes[4].Init( Vector( 0, 0, 1 ), -1 );
	planes[5].Init( Vector( 0, 0, -1 ), -1 );
	culler.SetBaseFrustumPlanes( planes );
	Require( culler.HasBaseFrustum() && !culler.HasInclusionVolume() && !culler.HasExclusionFrustum(),
		"base-only regression setup" );
	Require( culler.IsValid(), "base-only frustum must stay active when the inclusion volume is unavailable" );

	CheckBox( culler, Vector( 0, 0, 0 ), Vector( .25f, .25f, .25f ), true );
	CheckBox( culler, Vector( 0, 0, 0 ), Vector( 2, 2, 2 ), true );
	// Preserve boxes touching every face; reject only completely outside boxes.
	for ( int axis = 0; axis < 3; ++axis )
	{
		for ( int sign = -1; sign <= 1; sign += 2 )
		{
			Vector center( 0, 0, 0 );
			center[axis] = sign * 1.25f;
			CheckBox( culler, center, Vector( .25f, .25f, .25f ), true );
			center[axis] = sign * 1.5f;
			CheckBox( culler, center, Vector( .25f, .25f, .25f ), false );
		}
	}

	CVolumeCuller cached = culler;
	Require( cached.IsValid(), "copying to the view cache must preserve base-only validity" );
	CheckBox( cached, Vector( 5, 0, 0 ), Vector( 1, 1, 1 ), false );
	culler.SetBaseFrustumPlanes( NULL );
	Require( !culler.IsValid(), "removing the only volume must deactivate the culler" );

	culler.SetInclusionVolumePlanes( planes, 6 );
	Require( culler.IsValid(), "inclusion-only culler must stay active" );
	CheckBox( culler, Vector( 0, 0, 0 ), Vector( .25f, .25f, .25f ), true );
	CheckBox( culler, Vector( 5, 0, 0 ), Vector( 1, 1, 1 ), false );
	culler.Clear();
	culler.SetExclusionFrustumPlanes( planes );
	Require( culler.IsValid(), "exclusion-only culler must stay active" );
	CheckBox( culler, Vector( 0, 0, 0 ), Vector( .25f, .25f, .25f ), false );
	CheckBox( culler, Vector( 5, 0, 0 ), Vector( 1, 1, 1 ), true );
	culler.Clear();
	Require( !culler.IsValid(), "Clear must reset all volumes" );
	std::puts( "CSM culling: base-only validity, cached copy, all six boundaries and optional volumes passed" );
}
