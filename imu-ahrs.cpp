// For efficiency in compiling, this is the only file that uses the
// Eigen lirbary and the 'vector' type we made from it.
// Install libraries first :
//sudo apt-get install libi2c-dev libeigen3-dev libboost-program-options-dev

#include "vector.h"
#include "version.h"
#include "prog_options.h"
#include "minimu9.h"
#include "exceptions.h"
#include "pacer.h"
#include <iostream>
#include <iomanip>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include <system_error>
#include <chrono>

// TODO: print warning if accelerometer magnitude is not close to 1 when starting up

// An Euler angle could take 8 chars: -234.678, but usually we only need 6.
float field_width = 6;
#define FLOAT_FORMAT std::fixed << std::setprecision(3) << std::setw(field_width)

std::ostream & operator << (std::ostream & os, const vector & vector)
{
  return os << FLOAT_FORMAT << vector(0) << ' '
            << FLOAT_FORMAT << vector(1) << ' '
            << FLOAT_FORMAT << vector(2);
}

std::ostream & operator << (std::ostream & os, const matrix & matrix)
{
  return os << (vector)matrix.row(0) << ' '
            << (vector)matrix.row(1) << ' '
            << (vector)matrix.row(2);
}

std::ostream & operator << (std::ostream & os, const quaternion & quat)
{
  return os << FLOAT_FORMAT << quat.w() << ' '
            << FLOAT_FORMAT << quat.x() << ' '
            << FLOAT_FORMAT << quat.y() << ' '
            << FLOAT_FORMAT << quat.z();
}

typedef void rotation_output_function(quaternion & rotation);

void output_quaternion(quaternion & rotation)
{
  std::cout << rotation;
}

void output_matrix(quaternion & rotation)
{
  std::cout << rotation.toRotationMatrix();
}

void output_euler(quaternion & rotation)
{
  std::cout << (vector)(rotation.toRotationMatrix().eulerAngles(2, 1, 0)
                        * (180 / M_PI));
}

void stream_raw_values(imu & imu)
{
  imu.enable();
  while(1)
  {
    imu.read_raw();
    printf("%7d %7d %7d  %7d %7d %7d  %7d %7d %7d\n",
           imu.m[0], imu.m[1], imu.m[2],
           imu.a[0], imu.a[1], imu.a[2],
           imu.g[0], imu.g[1], imu.g[2]
      );
    usleep(20*1000);
  }
}

//! Uses the acceleration and magnetic field readings from the compass
// to get a noisy estimate of the current rotation matrix.
// This function is where we define the coordinate system we are using
// for the ground coords:  North, East, Down.
matrix rotation_from_compass(const vector & acceleration, const vector & magnetic_field)
{
  vector down = acceleration;     // Could also be negative, depending in the orientation of the board
  vector east = down.cross(magnetic_field); // actually it's magnetic east
  vector north = east.cross(down);

  east.normalize();
  north.normalize();
  down.normalize();

  matrix r;
  r.row(0) = north;
  r.row(1) = east;
  r.row(2) = down;
  return r;
}

typedef void fuse_function(quaternion & rotation, float dt, const vector & angular_velocity,
  const vector & acceleration, const vector & magnetic_field);

void fuse_compass_only(quaternion & rotation, float dt, const vector& angular_velocity,
  const vector & acceleration, const vector & magnetic_field)
{
  // Implicit conversion of rotation matrix to quaternion.
  rotation = rotation_from_compass(acceleration, magnetic_field);
}

// Uses the given angular velocity and time interval to calculate
// a rotation and applies that rotation to the given quaternion.
// w is angular velocity in radians per second.
// dt is the time.
void rotate(quaternion & rotation, const vector & w, float dt)
{
  // Multiply by first order approximation of the
  // quaternion representing this rotation.
  rotation *= quaternion(1, w(0)*dt/2, w(1)*dt/2, w(2)*dt/2);
  rotation.normalize();
}

void fuse_gyro_only(quaternion & rotation, float dt, const vector & angular_velocity,
  const vector & acceleration, const vector & magnetic_field)
{
  rotate(rotation, angular_velocity, dt);
}

void fuse_default(quaternion & rotation, float dt, const vector & angular_velocity,
  const vector & acceleration, const vector & magnetic_field)
{
  vector correction = vector(0, 0, 0);

  if (fabs(acceleration.norm() - 1) <= 0.3)
  {
    // The magnetidude of acceleration is close to 1 g, so
    // it might be pointing up and we can do drift correction.

    const float correction_strength = 1;

    matrix rotation_compass = rotation_from_compass(acceleration, magnetic_field);
    matrix rotation_matrix = rotation.toRotationMatrix();

    correction = (
      rotation_compass.row(0).cross(rotation_matrix.row(0)) +
      rotation_compass.row(1).cross(rotation_matrix.row(1)) +
      rotation_compass.row(2).cross(rotation_matrix.row(2))
      ) * correction_strength;

  }

  rotate(rotation, angular_velocity + correction, dt);
}

void ahrs(imu & imu, fuse_function * fuse, rotation_output_function * output)
{
  imu.load_calibration();
  imu.enable();
  imu.measure_offsets();

  // The quaternion that can convert a vector in body coordinates
  // to ground coordinates when it its changed to a matrix.
  quaternion rotation = quaternion::Identity();

  // Set up a timer that expires every 20 ms.
  pacer loop_pacer;
  loop_pacer.set_period_ns(20000000);

  auto start = std::chrono::steady_clock::now();
  while(1)
  {
    auto last_start = start;
    start = std::chrono::steady_clock::now();
    std::chrono::nanoseconds duration = start - last_start;
    float dt = duration.count() / 1e9;
    if (dt < 0){ throw std::runtime_error("Time went backwards."); }

    vector angular_velocity = imu.read_gyro();
    vector acceleration = imu.read_acc();
    vector magnetic_field = imu.read_mag();

    fuse(rotation, dt, angular_velocity, acceleration, magnetic_field);

   //output(rotation);
    matrix m = rotation.toRotationMatrix();
    float northx = m(0,0);
    float eastx =  m(1,0);
    float heading = atan2(northx,eastx)*(180 / M_PI) - 90;
    if (heading < 0) heading += 360;
    //vector euler = (m.eulerAngles(2, 1, 0) * (180 / M_PI));
    float nx= acceleration(0);float ny = acceleration(1);float nz = acceleration(2);
    float roll =atan2(ny, nz)*60;

    float pitch =atan2(nx,nz)*60;

   // std::cout << " Acc:" << acceleration << " Mag:" << magnetic_field << " N:"<<northx<<" E:"<<eastx<< "  \r";
   std::cout << " Acc:" << acceleration <<  " N:"<<northx<<" E:"<<eastx<<" Hdg:"<< heading<<" Roll:"<<roll<<" Pitch:"<< pitch << "  \r";
   //std::cout <<  " N:"<<northx<<" E:"<<eastx<<" Hdg:"<< heading << "  \r";
    loop_pacer.pace();
  }
}

int main_with_exceptions(int argc, char **argv)
{

  sensor_set set;
  set.mag = set.acc = set.gyro = true;

  minimu9::comm_config config = minimu9::auto_detect("/dev/i2c-1");

  minimu9::handle imu;
  imu.open(config);
  rotation_output_function* output;
  output = &output_matrix;
  //output =&output_euler;

  // Figure out the basic operating mode and start running.

    //stream_raw_values(imu);
    ahrs(imu, &fuse_default, output);



  return 0;
}

int main(int argc, char ** argv)
{
  try
  {
    main_with_exceptions(argc, argv);
  }
  catch(const std::system_error & error)
  {
    std::string what = error.what();
    const std::error_code & code = error.code();
    std::cerr << "Error: " << what << " (" << code << ")" << std::endl;
    return 2;
  }
  catch(const std::exception & error)
  {
    std::cerr << "Error: " << error.what() << std::endl;
    return 9;
  }
}
