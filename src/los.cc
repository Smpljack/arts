/*
Outline of interpolation functions:

There are two types of interpolations that we want to do:
1. Interpolation of some field at a set of points, for example, along the LOS.
2. To interpolate an input field to the calculation grids used, for example,
   to determine the temperatures at the pressure and latitude grid selected
   from a climatology. The interpolation is here done for all grid crossings.

To distinguish clearly these different cases, the interpolation of the kind in
case 2 will be denoted as re-sampling, and function names for case 1 will start
with "interp", while for case the names will start with "resample".

Both interp and resample functions will take index positions as input
(instead of physical positions). For example, the index position 6.5 means that
a position is exactly between the points with index 6 and 7, index position 5 
is exactly at the point with index 5 etc. [! A more stringent description
should be added. !]

Only linear interpolation will be implemented.

No resizing of vectors, matrices and tensors will be made inside the
interpolation functions. The functions assert that the arguments have 
consistent sizes.

Details for interp-functions (case 1):
The interpolation positions are given as a number of vectors, one for each
dimension that can exist (for example, pressure/altitude, latitude and
longitude). The length of these vectors must be equal, with the exception
that an empty vector (length zero) means that that dimension is not specified.
   For cases there the interpolation is performed along all dimensions, general
functions can be made, and these are called interp_1D, interp_2D, interp_3D.
The output of these functions are throughout a vector, where the length equals
the number of positions for which interpolation is performed. For example,
the function interp_3D can be used to interpolate the temperature field
(stored as a Tensor3) to the LOS. 
  On the other hand, there are cases when the interpolation is not performed 
along all dimensions. A typical example is interpolation of the absorption
tensor to get the absorption at the points along the LOS, where no 
interpolation is made in the frequency dimension. For such interpolation cases
special functions will be made (e.g. interp_abs2los). To make general 
functions would be too messy and inefficient.
  Both for the general and special cases, the interpolation functions adapt
automatically to the present dimensionality of the simulations. For example, 
if the simulations are 2D, indicated by that the vector with longitude 
positions is empty and the number of pages of the temperature tensor is 1
(consistency between these two criteria is checked), the longitude dimension is
ignored during the interpolation.

Details for resample-functions (case 2):
Resampling corresponds to an interpolation over all involved dimensions. The
grids are given as individual vectors. An empty vector indicates (as above)
that the corresponding dimension is not specified. Interpolation is performed
for all possible combinations between the vectors, that is, all grid crossings
(as expected). The functions adapt automatically to the dimensionality of the
simulations (as above).
*/


#include <math.h>
#include "arts.h"
#include "matpackI.h"
#include "matpackIII.h"
#include "messages.h"          
#include "auto_wsv.h"          

extern const Numeric DEG2RAD;
extern const Numeric RAD2DEG;
extern const Numeric EARTH_RADIUS;

struct LOS {
  Index     dim;
  Index     np;
  Index     i_start;
  Index     i_stop;
  Vector    p;
  Vector    z;
  Vector    ip_p;
  Vector    lat;
  Vector    ip_lat;
  Vector    lon;
  Vector    ip_lon;
  Vector    l_step;
  Index     background;
  Index     ground;
  Index     i_ground;
};



/** Returns the first value of a vector.

    \return       The first value of x.
    \param    x   A vector.

    \author Patrick Eriksson 
    \date   2000-06-27
*/
Numeric first2( ConstVectorView x )
{
  return x[0]; 
}



/** Returns the last value of a vector.

    \return      The last value of x.
    \param   x   A vector.

    \author Patrick Eriksson 
    \date   2000-06-27
*/
Numeric last2( ConstVectorView x )
{
  return x[x.nelem()-1]; 
}



void assert_size( 
	ConstVectorView   x,
	const Index&      l ) 
{
  assert( x.nelem() == l );
}



void assert_size( 
	ConstMatrixView   x,
	const Index&      nrows,
	const Index&      ncols ) 
{
  assert( x.ncols() == ncols );
  assert( x.nrows() == nrows );
}



void assert_size( 
	const Tensor3&    x,
	const Index&      npages,
	const Index&      nrows,
	const Index&      ncols ) 
{
  assert( x.ncols() == ncols );
  assert( x.nrows() == nrows );
  assert( x.npages() == npages );
}



void assert_maxdim_of_tensor(
	const Tensor3&   x,
	const Index&     dim )
{
  assert( dim >= 1 );
  assert( dim <= 3 );

  if( dim == 1 )
    {
      assert( x.nrows() == 1 );
      assert( x.npages() == 1 );
    }
  else if( dim == 2 )
    assert( x.npages() == 1 );
}



void get_interp_weights (
	      ArrayOfIndex&   ii,
	      Vector&         w,
	ConstVectorView       y,
	ConstVectorView       ip_x )
{
  // Sizes
  const Index   n_y   = y.nelem();
  const Index   n_out = ip_x.nelem();

  // Asserts
  assert( ii.nelem() == n_out );
  assert( w.nelem() == n_out );

  for( Index ix=0; ix<n_out; ix++ )
    {
      ii[ix] = Index( floor( ip_x[ix] ) );
      assert( ii[ix] >= 0 );
      w[ix]  = ip_x[ix] - Numeric( ii[ix] );
      
      // If w is very small (< 1e-6), treat it to be 0.
      // If w=0, we can be at the end point.
      if( w[ix] < 1e-6 ) 
	{
	  assert( ii[ix] < n_y );
	  w[ix] = 0;
	}
      else
	  assert( ii[ix] < n_y-1 );
    }
}	     



Index get_dim_for_interp (
	ConstVectorView   ip_p,
	ConstVectorView   ip_lat,
	ConstVectorView   ip_lon )
{
  if( ip_lat.nelem() == 0 )
    {
      assert( ip_lon.nelem() == 0 );
      return 1;
    }
  else if( ip_lon.nelem() == 0 )
    {
      assert( ip_p.nelem() == ip_lat.nelem() );
      return 2;
    }
  else
    {
      assert( ip_p.nelem() == ip_lat.nelem() );
      assert( ip_p.nelem() == ip_lon.nelem() );
      return 3;
    }
}



void interp_1D (
	      Vector&     y,
	ConstVectorView   yi,
	ConstVectorView   ip_x )
{
  // Sizes
  const Index   n_in = yi.nelem();
  const Index   n_out = ip_x.nelem();

  // Asserts
  assert( y.nelem() == n_out );

  // Interpolatation weights
  ArrayOfIndex   ii(n_out); 
  Vector         w(n_out);
  get_interp_weights( ii, w, yi, ip_x );

  for( Index ix=0; ix<n_out; ix++ )
    {
      if( w[ix] == 0 )   // No interpolation, just copy data
	y[ix] = yi[ii[ix]];
      else
	y[ix] = (1-w[ix])*yi[ii[ix]] + w[ix]*yi[ii[ix]+1];
    }
}	     



void interp_abs2los (
	      Matrix&     abs_out,
	const Tensor3&    abs_in,
	ConstVectorView   ip_p,
	ConstVectorView   ip_lat,
	ConstVectorView   ip_lon )
{
  // Get dimension and check consistency of index vectors
  Index   dim = get_dim_for_interp( ip_p, ip_lat, ip_lon );

  // Check that the input absorption tensor match the found dimension
  assert_maxdim_of_tensor( abs_in, dim+1 );

  // Check that return matrix has the correct size
  const Index   n  = ip_p.nelem();
  const Index   nv = abs_in.ncols();
  assert_size( abs_out, n, nv );

  // Get interpolation weights for pressure/altitude dimension
  ArrayOfIndex   ii1(n); 
  Vector         w1(n);
  get_interp_weights( ii1, w1, abs_in(0,Range(joker),0), ip_p );

  // Interpolate
  Index iv;
  Numeric wv;
  if( dim == 1 )
    {
      for( Index ix=0; ix<n; ix++ )
	{
	  wv = w1[ix];
	  if( wv == 0 )   // No interpolation, just copy data
	    {
	      for( iv=0; iv<nv; iv++ )
		abs_out(ix,iv) = abs_in(0,ii1[ix],iv);
	    }
	  else
	    {
	      for( iv=0; iv<nv; iv++ )
	        abs_out(ix,iv) = (1-wv) * abs_in(0,ii1[ix],iv) +
	                             wv * abs_in(0,ii1[ix]+1,iv);
	    }
	}
    }
  else
    throw runtime_error("Absorption interpolation only implemented for 1D." );
}



void los_1d_geom (
	      Numeric&    z_tan,
              Index&      nz,
	      Vector&     z,
	      Vector&     ip_z,
	      Vector&     lat,
	      Vector&     l_step,
 	      Index&      ground,
	const Numeric&    z_ground,
	ConstVectorView   z_abs,
	const Numeric&    r_geoid,
	const Numeric&    l_max,
	const Numeric&    z_plat,
	const Numeric&    za )
{
  // Asserts
  assert( z_ground >= z_abs[0] );
  assert( z_ground < last2(z_abs) );
  assert( z_abs.nelem() > 1 );
  assert( l_max > 0 );
  assert( z_plat >= z_ground );
  assert( za >= 0 );
  assert( za <= 180 );

  // Guess a value for all index return arguments.
  z_tan   = 9999e3;
  nz      = 0;
  ground  = 0;  

  // Get highest absorption altitude and length of z_abs
  const Index     n_zabs = z_abs.nelem();
  const Numeric   z_max  = z_abs[n_zabs-1];

  // Determine the lowest point of the LOS, z1, and the zenith angle of the LOS
  // at this point, za1. The tangent altitude, ground flag and some other 
  // variables are set on the same time.
  // The latitude distance from the sensor and z1 is denoted as lon0.
  // The case with downward observation from inside the atmosphere nees special
  // treatment, handled by the flag do_down.
  //
  Numeric z1, za1, lat0;
  Index   do_down = 0;
  //
  if( za <= 90 )   // Upward observation (no tangent point)
    {
      z1   = z_plat;
      za1  = za;
      lat0 = 0;
    }
  else              // Downward observation (limb sounding)
    {
      z_tan = (r_geoid+z_plat) * sin(DEG2RAD*za) - r_geoid; 
      if( z_tan >= z_ground )   // No intersection with the ground
	{
	  z1   = z_tan;
	  za1  = -90;
          lat0 = za - 90.0;
	}
      else                       // Intersection with ground
	{
	  ground = 1;
	  z1     = z_ground;
	  za1    = -RAD2DEG * asin( (r_geoid+z_tan) / (r_geoid+z_ground) );
          lat0   = za - za1 - 180.0;
	}
      if( z_plat < z_max )
	do_down = 1;
    }

  // The return vectors are set to be empty if z1 >= z_max
  if( z1 >= z_max )
    {
      z.resize(0);
      ip_z.resize(0);
      l_step.resize(0);
      lat.resize(0);
    }

  // Do anything only if z1 is inside the atmosphere
  else
    {
      // Create vectors for special points.
      // Special points are z1 and z_plat (if do_down).
      // This vector shall start with z1 and end with a dummy value > z_max.
      const Index    n_special = 1 + do_down;  // The dummy value is ignored
            Vector   z_special(n_special+1); 
      //
      z_special[0] = z1;
      if( do_down )
	z_special[1] = z_plat;
      z_special[do_down+1] = z_max*2;

      // Determine index of first z_abs above z1, i_above.
      Index   i_above;
      for( i_above=0; z_abs[i_above]<=z1; i_above++ ) {}

      // Create a vector containing z_special and z_abs levels above z1 (zs).
      // The altitudes shall be sorted and there shall be no duplicates.
      // It is assumed that the first shall be taken from z_special.
      Index    n_zs = n_zabs - i_above + n_special; 
      Vector   zs(n_zs);
      Index    i1, i2=1;
      //
      zs[0] = z_special[0];
      n_zs  = 1;            // n_zs counts now the number of values moved to zs
      for( i1=i_above; i1<n_zabs; i1++  )
	{
	  // Check if values from z_special shall be copied
	  for( ; z_special[i2] <= z_abs[i1]; i2++ )
	    {
	      if( zs[n_zs] != z_special[i2] )
		{
		  zs[n_zs] = z_special[i2];
		  n_zs++;
		}
	    }
	  // Copy next z_abs
	  if( zs[n_zs] != z_abs[i1] )
	    {
	      zs[n_zs] = z_abs[i1];
	      n_zs++;
	    }
	}

      // Calculate the length along the LOS from z1 (ls) and the number of
      // LOS steps needed to reach next altitude in zs (ns).
      // The geometric calculations can go wrong if float is used, and to
      // avoid this, the variables a-e are hard-coded to be double.
      Vector         ls(n_zs);
      ArrayOfIndex   ns(n_zs-1);               
      double         a, b, c, d, e;
      Index          n_sum = 0;       // n_sum is sum(ns)
      //
      // Handle first point seperately
      zs[0] = zs[0]; 
      ls[0] = 0; 
      //
      // Loop zs
      a  = cos( DEG2RAD * za1 );
      b  = sin( DEG2RAD * za1 );
      c  = ( r_geoid + z1 ) * b;
      c *= c;
      d  = ( r_geoid + z1 ) * a;
      for( i1=1; i1<n_zs; i1++ )
	{
	  e        = r_geoid + zs[i1];
	  ls[i1]   = sqrt( e*e - c ) - d;
          ns[i1-1] = Index( ceil( (ls[i1]-ls[i1-1])/l_max ) );
          n_sum   += ns[i1-1];
	}
      n_sum += 1;   // To account for the point at the last z_abs level

      // The length of the z and lat vectors is n_sum
      nz = n_sum;
      
      // Create the return vectors.
      z.resize(nz);
      ip_z.resize(nz);
      l_step.resize(nz-1);
      lat.resize(nz);
      //
      Numeric   dl, l;              // Different lengts along the LOS
      Numeric   zv;                 // Next altitude
      c = r_geoid + z1;  
      d = c * c;
      n_sum = 0;                 // Re-use this variable
      //
      for( i1=0; i1<n_zs-1; i1++ )
	{
          dl = (ls[i1+1] - ls[i1]) / ns[i1];
	  for( i2=0; i2<ns[i1]; i2++ )
	    {
	      l = ls[i1] + i2*dl;
	      if( i2 == 0 )
		zv = zs[i1];
	      else
		zv = sqrt( d + l*l + 2 * c * l * a ) - r_geoid;
	      z[n_sum+i2]      = zv;
	      l_step[n_sum+i2] = dl;
	      ip_z[n_sum+i2]   = i_above - 1 + ( zv - z_abs[i_above-1]) /
                                           (z_abs[i_above] - z_abs[i_above-1]);
	      lat[n_sum+i2]    = lat0 + RAD2DEG*asin( l*b / (r_geoid+zv) ); 
	    }

          // Increase n_sum with the points done
          n_sum += ns[i1];

          // Check if i_above shall be increased
	  if( zs[i1] >= z_abs[i_above] )
	    i_above++;
	}
      
      // Put in uppermost z_abs level that is not covered above
      z[n_sum]    = z_abs[n_zabs-1];
      ip_z[n_sum] = n_zabs-1;
      lat[n_sum]  = lat0 + RAD2DEG*asin( ls[n_zs-1]*b / (r_geoid+z[n_sum]) ); 
    }
}



void los_1d (
	      LOS&        los,
              Numeric&    z_tan,
	const Numeric&    z_ground,
	ConstVectorView   z_abs,
	ConstVectorView   p_abs,
	const Numeric&    r_geoid,
	const Numeric&    l_max,
	const Numeric&    z_plat,
	const Numeric&    za,
        const Index&      refr_on,
        const Index&      blackbody_ground,
	const Index&      scattering_on )
{
  // Check input 
  // za is inside 0-180

  // LOS variables that always are the same
  los.dim = 1;
  los.ip_lat.resize(0);
  los.lon.resize(0);
  los.ip_lon.resize(0);
  los.i_ground   = 0;

  // Set background to CBGR
  los.background = 0;

  // Do stuff that differ between with and without refraction
  if( refr_on )
    throw runtime_error(
                  "1D LOS calculations with refraction not yet implemented" );
  else
    los_1d_geom ( z_tan, los.np, los.z, los.ip_p, los.lat, los.l_step, 
               los.ground, z_ground, z_abs, EARTH_RADIUS, l_max, z_plat, za );

  // Set i_start and i_stop assuming blackbody_ground and scattering are 0, and
  // not blackbody ground.
  if( za <= 90 )
    los.i_stop = 0;
  else
    los.i_stop = los.np - 1;
  los.i_start = los.np - 1;

  // Downward observation from inside the atmosphere needs special treatment.
  if( za>90 && z_plat<last2(z_abs) )
    for( los.i_stop=0; los.z[los.i_stop]!=z_plat; los.i_stop++ ) {}

  // Ignore part of the LOS before ground reflection if blackbody ground.
  // The start index is then always 0.
  // If i_stop deviates from np-1, the vectors shall be truncated (corresponds
  // to downward observation from within the atmosphere and blxackbody ground).
  if( los.ground && blackbody_ground )
    {
      los.background = 1;
      los.i_start    = 0;

      // Truncate vetors
      if( los.i_stop < los.np-1 )
	{
          const Index    n = los.i_stop+1;
	        Vector   dummy(n);
	  los.np = n;
	  dummy = los.z[ Range(0,n) ];
	  los.z.resize(n);
	  los.z = dummy;
	  dummy = los.ip_p[ Range(0,n) ];
	  los.ip_p.resize(n);
	  los.ip_p = dummy;
	  dummy = los.lat[ Range(0,n) ];
	  los.lat.resize(n);
	  los.lat = dummy;
	  dummy.resize(n-1);
	  dummy = los.l_step[ Range(0,n-1) ];
	  los.l_step.resize(n-1);
	  los.l_step = dummy;
	}
    }

  // Without scattering
  if( scattering_on )
    throw runtime_error(
                   "1D LOS calculations with scattering not yet implemented" );

  // Convert altitudes to pressures
  los.p.resize(los.np);
  interp_1D ( los.p, p_abs, los.ip_p ); 
}



void test_new_los()
{
  LOS los;
 
  Numeric z_tan, z_ground, l_max, z_plat, za;

  z_ground = 200;
  l_max    = 500;
  z_plat   = 1e3;
  za       = 45;
  
  Vector z_abs( 0, 11, 1e3 );
  Vector p_abs( 0, 11, 1 );

  los_1d( los, z_tan, z_ground, z_abs, p_abs, EARTH_RADIUS, l_max, z_plat, za,
                                                                     0, 1, 0 );

  cout << "z = \n" << los.z << "\n";
  cout << "p = \n" << los.p << "\n";
  //cout << "ip_p = \n" << los.ip_p << "\n";
  cout << "lat = \n" << los.lat << "\n";
  cout << "l_step = \n" << los.l_step << "\n";
  cout << "nz      = " << los.np << "\n";
  cout << "i_start = " << los.i_start << "\n";
  cout << "i_stop  = " << los.i_stop << "\n";
  cout << "bground = " << los.background << "\n";
  cout << "ground  = " << los.ground << "\n";
  cout << "i_ground= " << los.i_ground << "\n";
  cout << "z_tan   = " << z_tan/1e3 << " km\n";


  Tensor3 abs_in(1,z_abs.nelem(),2,0.0);
  for ( Index j=0; j<abs_in.nrows(); ++j )
    for ( Index k=0; k<abs_in.ncols()-1; ++k )
      abs_in(0,j,k) = j;
  cout << "abs_in = " << abs_in << "\n";

  Matrix abs_out(los.p.nelem(),2);
  
  interp_abs2los( abs_out, abs_in, los.ip_p, los.ip_lat, los.ip_lon );
  cout << "abs_out = \n" << abs_out << "\n";  
}
