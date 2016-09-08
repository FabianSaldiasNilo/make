// pinstart.cpp: utilities for creating a start shape from manually pinned points
//
// Copyright (C) 2005-2013, Stephen Milborrow

#include "stasm.h"

#include "opencv2/imgproc.hpp"

namespace stasm
{
// The following model was machine generated by running
// 5pointpose.R on the training shapes and their reflections.

static double EstYawFrom5PointShape(const double* x) // x has 10 elements (5 points)
{
    return  34.342
      -   7.0267 * MAX(0,      x[3] -  -0.34708)
      +   10.739 * MAX(0,  -0.34708 -      x[3])
      +   116.29 * MAX(0,      x[4] -   0.21454)
      -   159.56 * MAX(0,   0.21454 -      x[4])
      +   12.513 * MAX(0,      x[7] -    0.3384)
      +   7.2764 * MAX(0,    0.3384 -      x[7])
      +   260.14 * MAX(0,      x[3] -  -0.34708) * MAX(0,      x[5] - -0.010838)
      -   160.64 * MAX(0,      x[3] -  -0.34708) * MAX(0, -0.010838 -      x[5])
      -   284.88 * MAX(0,  -0.34708 -      x[3]) * MAX(0,      x[5] - -0.055581)
      +   654.54 * MAX(0,  -0.34708 -      x[3]) * MAX(0, -0.055581 -      x[5])
    ;
}

static void RotShapeInPlace(
    Shape& shape,     // io
    double rot,       // in: in-plane rotation angle in degrees, pos is anticlock
    double x,         // in: rotation origin
    double y)         // in
{
    CV_Assert(rot >= -360 && rot <= 360); // sanity check, 360 is arb

    const MAT rotmat =
        getRotationMatrix2D(cv::Point2f(float(x), float(y)), rot, 1.0);

    TransformShapeInPlace(shape, rotmat);
}

// If shape5 does not have 5 points, return rot and yaw of 0.
// Else assume that the following five points are present, in this order:
//   0 LEyeOuter
//   1 REyeOuter
//   2 CNoseTip
//   3 LMouthCorner
//   4 RMouthCorner

static void EstRotAndYawFrom5PointShape(
    double&     rot,                     // out
    double&     yaw,                     // out
    const Shape shape5)                  // in
{
    CV_Assert(shape5.rows == 5);

    Shape workshape(shape5.clone()); // local copy we can modify

    // Derotate shape5 using eye angle as estimate of in-plane rotation.
    // We rotate about the shape5 centroid.
    // TODO EstYawFrom5PointShape was trained on shapes without this
    // derotation, so must retrain the model for best results.

    rot = RadsToDegrees(-atan2(workshape(1, IY) - workshape(0, IY),
                               workshape(1, IX) - workshape(0, IX)));

    PossiblySetRotToZero(rot); // treat small Rots as zero Rots

    if (rot)
        RotShapeInPlace(workshape,
                        -rot,
                        SumElems(workshape.col(IX)) / 5,
                        SumElems(workshape.col(IY)) / 5);

    // mean-center x and y
    MAT X(workshape.col(IX)); X -= SumElems(X) / 5;
    MAT Y(workshape.col(IY)); Y -= SumElems(Y) / 5;

    // normalize so workshape size is 1
    double norm = 0;
    for (int i = 0; i < 5; i++)
        norm += SQ(X(i)) + SQ(Y(i));
    workshape /= sqrt(norm);

    yaw = EstYawFrom5PointShape(Buf(workshape));
}

static Shape PinMeanShape(  // align mean shape to the pinned points
    const Shape& pinned,    // in: at least two of these points must be set
    const Shape& meanshape) // in
{
    CV_Assert(pinned.rows == meanshape.rows);

    int ipoint, nused = 0;  // number of points used in pinned
    for (ipoint = 0; ipoint < meanshape.rows; ipoint++)
        if (PointUsed(pinned, ipoint))
            nused++;

    if (nused < 2)
        Err("Need at least two pinned landmarks");

    // Create an anchor shape (the pinned landmarks) and an alignment shape (the
    // points in meanshape that correspond to those pinned landmarks).  Do that by
    // copying the used points in pinned to pinned_used, and the corresponding
    // points in meanshape to meanused.

    Shape pinned_used(nused, 2), mean_used(nused, 2);
    int i = 0;
    for (ipoint = 0; ipoint < meanshape.rows; ipoint++)
        if (PointUsed(pinned, ipoint))
        {
            pinned_used(i, IX) = pinned(ipoint, IX);
            pinned_used(i, IY) = pinned(ipoint, IY);
            mean_used(i, IX)   = meanshape(ipoint, IX);
            mean_used(i, IY)   = meanshape(ipoint, IY);
            i++;
        }
    CV_Assert(i == nused);

    // transform meanshape to pose generated by aligning mean_used to pinned_used
    Shape TransformedShape(
                    TransformShape(meanshape, AlignmentMat(mean_used, pinned_used)));

    return JitterPointsAt00(TransformedShape);
}

static bool HaveCanonical5Points(
    const Shape& pinned17)           // in: pinned landmarks
{
    CV_Assert(pinned17.rows == 17);
    return PointUsed(pinned17, L17_LEyeOuter)    &&
           PointUsed(pinned17, L17_REyeOuter)    &&
           PointUsed(pinned17, L17_CNoseTip)     &&
           PointUsed(pinned17, L17_LMouthCorner) &&
           PointUsed(pinned17, L17_RMouthCorner);
}

static void CopyPoint(     // copy a point from oldshape to shape
    Shape&       shape,    // io
    const Shape& oldshape, // in
    int          i,        // in
    int          iold)     // in
{
    shape(i, IX) = oldshape(iold, IX);
    shape(i, IY) = oldshape(iold, IY);
}

static Shape Shape5(        // return a 5 point shape
    const Shape& pinned,    // in: pinned landmarks, canonical 5 points are best
    const Shape& meanshape) // in: used only if pinned landmarks are not canonical
{
    const Shape pinned17(Shape17(pinned));
    const Shape meanshape17(Shape17(meanshape));
    Shape newpinned17(pinned17);
    if (!HaveCanonical5Points(pinned17))
    {
        // Not canonical 5 point pinned landmarks.  Impute the missing points.
        // This is not an optimal situation but will at least allow estimation
        // of the pose from  an arb set of pinned landmarks.
        // TODO Only the Shape17 points are considered.

        newpinned17 = PinMeanShape(pinned17, meanshape17);
    }
    Shape shape5(5, 2); // 5 point shape

    CopyPoint(shape5, newpinned17, 0, L17_LEyeOuter);
    CopyPoint(shape5, newpinned17, 1, L17_REyeOuter);
    CopyPoint(shape5, newpinned17, 2, L17_CNoseTip);
    CopyPoint(shape5, newpinned17, 3, L17_LMouthCorner);
    CopyPoint(shape5, newpinned17, 4, L17_RMouthCorner);

    return shape5;
}

static void InitDetParEyeMouthFromShape( // fill in eye and mouth fields of detpar
    DetectorParameter& detpar,
    Shape&  shape)
{
    const Shape shape17(Shape17(shape));
    if (PointUsed(shape17, L17_LPupil))
    {
        detpar.lex = shape17(L17_LPupil, IX);
        detpar.ley = shape17(L17_LPupil, IY);
    }
    if (PointUsed(shape17, L17_RPupil))
    {
        detpar.rex = shape17(L17_RPupil, IX);
        detpar.rey = shape17(L17_RPupil, IY);
    }
    if (PointUsed(shape17, L17_CBotOfBotLip))
    {
        detpar.mouthx = shape17(L17_CBotOfBotLip, IX);
        detpar.mouthy = shape17(L17_CBotOfBotLip, IY);
    }
}

// We generated the startshape without using the face detector, now "back
// generate" the detpar (the position of this does not have to exactly
// match the detpar that would generate the startshape). This approach
// allows detpar to be handled uniformly in PinnedStartShapeAndRoi.

static DetectorParameter PseudoDetParFromStartShape(
    const Shape& startshape,
    double       rot,
    double       yaw,
    int          nmods)
{
    const Shape shape17(Shape17(startshape));
    const double lex    = shape17(L17_LPupil, IX);       // left eye
    const double ley    = shape17(L17_LPupil, IY);
    const double rex    = shape17(L17_RPupil, IX);       // right eye
    const double rey    = shape17(L17_RPupil, IY);
    const double mouthx = shape17(L17_CBotOfBotLip, IX); // mouth
    const double mouthy = shape17(L17_CBotOfBotLip, IY);

    CV_Assert(PointUsed(lex, ley));
    CV_Assert(PointUsed(rex, rey));
    CV_Assert(PointUsed(mouthx, mouthy));

    const double xeye = (lex + rex) / 2;                 // midpoint of eyes
    const double yeye = (ley + rey) / 2;
    const double eyemouth = PointDist(xeye, yeye, mouthx, mouthy);

    DetectorParameter detpar;

    detpar.x = .7 * xeye + .3 * mouthx;
    detpar.y = .7 * yeye + .3 * mouthy;
    detpar.width  = 2.0 * eyemouth;
    detpar.height = 2.0 * eyemouth;
    detpar.lex = lex;
    detpar.ley = ley;
    detpar.rex = rex;
    detpar.rey = rey;
    detpar.mouthx = mouthx;
    detpar.mouthy = mouthy;
    detpar.rot = rot;
    detpar.eyaw = DegreesAsEyaw(yaw, nmods); // determines what ASM model to use
    detpar.yaw = yaw;

    return detpar;
}

// Use the given pinned face landmarks to init the start shape.  The
// current implementation works best if the pinned landmarks are the five
// canonical pinned landmarks (viz. LEyeOuter, REyeOuter, CNoseTip,
// LMouthCorner, RMouthCorner).  This is because it was trained on those
// points.  But the routine also works if any two or more points are pinned.

void PinnedStartShapeAndRoi(   // use the pinned landmarks to init the start shape
    Shape&         startshape, // out: the start shape (in ROI frame)
    Image&         face_roi,   // out: ROI around face, possibly rotated upright
    DetectorParameter& detpar_roi, // out: detpar wrt to face_roi
    DetectorParameter& detpar,     // out: detpar wrt to img
    Shape&         pinned_roi, // out: pinned arg translated to ROI frame
    const Image&   img,        // in: the image (grayscale)
    const vec_Mod& mods,       // in: a vector of models, one for each yaw range
    const Shape&   pinned)     // in: manually pinned landmarks
{
    double rot, yaw;
    EstRotAndYawFrom5PointShape(rot, yaw,
                                Shape5(pinned, mods[0]->MeanShape_()));
    const EYAW eyaw = DegreesAsEyaw(yaw, NSIZE(mods));
    const int imod = EyawAsModIndex(eyaw, mods); // select ASM model based on yaw
    if (trace_g)
        lprintf("%-6.6s yaw %3.0f rot %3.0f ", EyawAsString(eyaw), yaw, rot);
    pinned_roi = pinned;    // use pinned_roi as a temp shape we can change
    Image workimg(img);     // possibly flipped image
    if (IsLeftFacing(eyaw)) // left facing? (our models are for right facing faces)
    {
        pinned_roi = FlipShape(pinned_roi, workimg.cols);
        FlipImgInPlace(workimg);
    }
    const Mod* mod = mods[ABS(imod)];
    startshape = PinMeanShape(pinned_roi, mod->MeanShape_());
    startshape = mod->ConformShapeToMod_Pinned_(startshape, pinned_roi);
    detpar = PseudoDetParFromStartShape(startshape, rot, yaw, NSIZE(mods));
    if (IsLeftFacing(eyaw))
        detpar.rot *= -1;
    FaceRoiAndDetectorParameter(face_roi, detpar_roi, workimg, detpar, false);
    startshape = ImgShapeToRoiFrame(startshape, detpar_roi, detpar);
    pinned_roi = ImgShapeToRoiFrame(pinned_roi, detpar_roi, detpar);
    // following line not strictly necessary because don't actually need eyes/mouth
    InitDetParEyeMouthFromShape(detpar_roi, startshape);
    if (IsLeftFacing(eyaw))
    {
        detpar = FlipDetPar(detpar, img.cols);
        detpar.rot = -detpar.rot;
        detpar_roi.x += 2. * (face_roi.cols/2. - detpar_roi.x);
    }
}

} // namespace stasm
