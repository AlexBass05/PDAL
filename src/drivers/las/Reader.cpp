/******************************************************************************
* Copyright (c) 2011, Michael P. Gerlek (mpg@flaxen.com)
*
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following
* conditions are met:
*
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above copyright
*       notice, this list of conditions and the following disclaimer in
*       the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of Hobu, Inc. or Flaxen Geo Consulting nor the
*       names of its contributors may be used to endorse or promote
*       products derived from this software without specific prior
*       written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
* FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
* COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
* OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
* AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
* OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
* OF SUCH DAMAGE.
****************************************************************************/

#include <pdal/drivers/las/Reader.hpp>

#ifdef PDAL_HAVE_LASZIP
#include <laszip/lasunzipper.hpp>
#endif

#include <pdal/FileUtils.hpp>
#include <pdal/drivers/las/Header.hpp>
#include <pdal/drivers/las/Iterator.hpp>
#include <pdal/drivers/las/VariableLengthRecord.hpp>
#include "LasHeaderReader.hpp"
#include <pdal/PointBuffer.hpp>
#include "ZipPoint.hpp"

namespace pdal { namespace drivers { namespace las {


Reader::Reader(const Options& options)
    : ReaderBase(options)
    , m_filename(options.getValueOrThrow<std::string>("filename"))
{
    return;
}


Reader::Reader(const std::string& filename)
    : ReaderBase(Options::none())
    , m_filename(filename)
{
    return;
}


void Reader::initialize()
{
    pdal::Reader::initialize();

    std::istream* stream = FileUtils::openFile(m_filename);

    LasHeaderReader lasHeaderReader(m_lasHeader, *stream);
    lasHeaderReader.read( getSchemaRef() );

    this->setBounds(m_lasHeader.getBounds());
    this->setNumPoints(m_lasHeader.GetPointRecordsCount());
    
    // If the user is already overriding this by setting it on the stage, we'll 
    // take their overridden value
    if (getSpatialReference().getWKT(pdal::SpatialReference::eCompoundOK).empty())
    {
        SpatialReference srs;
        m_lasHeader.getVLRs().constructSRS(srs);
        setSpatialReference(srs);
    }

    FileUtils::closeFile(stream);

    return;
}


const Options Reader::getDefaultOptions() const
{
    Option option1("filename", "", "file to read from");
    Options options(option1);
    return options;
}


const std::string& Reader::getFileName() const
{
    return m_filename;
}


int Reader::getMetadataRecordCount() const
{
    return m_metadataRecords.size();
}


const MetadataRecord& Reader::getMetadataRecord(int index) const
{
    return m_metadataRecords[index];
}


MetadataRecord& Reader::getMetadataRecordRef(int index)
{
    return m_metadataRecords[index];
}


PointFormat Reader::getPointFormat() const
{
    return m_lasHeader.getPointFormat();
}


boost::uint8_t Reader::getVersionMajor() const
{
    return m_lasHeader.GetVersionMajor();
}


boost::uint8_t Reader::getVersionMinor() const
{
    return m_lasHeader.GetVersionMinor();
}


const std::vector<VariableLengthRecord>& Reader::getVLRs() const
{
    return m_lasHeader.getVLRs().getAll();
}


boost::uint64_t Reader::getPointDataOffset() const
{
    return m_lasHeader.GetDataOffset();
}


bool Reader::isCompressed() const
{
    return m_lasHeader.Compressed();
}


pdal::StageSequentialIterator* Reader::createSequentialIterator() const
{
    return new SequentialIterator(*this);
}


pdal::StageRandomIterator* Reader::createRandomIterator() const
{
    return new RandomIterator(*this);
}


boost::uint32_t Reader::processBuffer(PointBuffer& data, std::istream& stream, boost::uint64_t numPointsLeft, LASunzipper* unzipper, ZipPoint* zipPoint) const
{
    // we must not read more points than are left in the file
    const boost::uint64_t numPoints64 = std::min<boost::uint64_t>(data.getCapacity(), numPointsLeft);
    const boost::uint32_t numPoints = (boost::uint32_t)std::min<boost::uint64_t>(numPoints64, std::numeric_limits<boost::uint32_t>::max());

    const LasHeader& lasHeader = getLasHeader();
    const Schema& schema = data.getSchema();
    const PointFormat pointFormat = lasHeader.getPointFormat();

    const PointIndexes indexes(schema, pointFormat);

    const bool hasTime = Support::hasTime(pointFormat);
    const bool hasColor = Support::hasColor(pointFormat);
    const int pointByteCount = Support::getPointDataSize(pointFormat);

    boost::uint8_t* buf = new boost::uint8_t[pointByteCount * numPoints];

    if (zipPoint)
    {
#ifdef PDAL_HAVE_LASZIP
        boost::uint8_t* p = buf;

        bool ok = false;
        for (boost::uint32_t i=0; i<numPoints; i++)
        {
            ok = unzipper->read(zipPoint->m_lz_point);
            if (!ok)
            {
                std::ostringstream oss;
                const char* err = unzipper->get_error();
                if (err==NULL) err="(unknown error)";
                oss << "Error reading compressed point data: " << std::string(err);
                throw pdal_error(oss.str());
            }

            memcpy(p, zipPoint->m_lz_point_data.get(), zipPoint->m_lz_point_size);
            p +=  zipPoint->m_lz_point_size;
        }
#else
                throw pdal_error("LASzip is not enabled for this pdal::drivers::las::Reader::processBuffer");
#endif
    }
    else
    {
        Utils::read_n(buf, stream, pointByteCount * numPoints);
    }

    for (boost::uint32_t pointIndex=0; pointIndex<numPoints; pointIndex++)
    {
        boost::uint8_t* p = buf + pointByteCount * pointIndex;

        // always read the base fields
        {
            const boost::int32_t x = Utils::read_field<boost::int32_t>(p);
            const boost::int32_t y = Utils::read_field<boost::int32_t>(p);
            const boost::int32_t z = Utils::read_field<boost::int32_t>(p);
            const boost::uint16_t intensity = Utils::read_field<boost::uint16_t>(p);
            const boost::uint8_t flags = Utils::read_field<boost::uint8_t>(p);
            const boost::uint8_t classification = Utils::read_field<boost::uint8_t>(p);
            const boost::int8_t scanAngleRank = Utils::read_field<boost::int8_t>(p);
            const boost::uint8_t user = Utils::read_field<boost::uint8_t>(p);
            const boost::uint16_t pointSourceId = Utils::read_field<boost::uint16_t>(p);

            const boost::uint8_t returnNum = flags & 0x07;
            const boost::uint8_t numReturns = (flags >> 3) & 0x07;
            const boost::uint8_t scanDirFlag = (flags >> 6) & 0x01;
            const boost::uint8_t flight = (flags >> 7) & 0x01;

            data.setField<boost::uint32_t>(pointIndex, indexes.X, x);
            data.setField<boost::uint32_t>(pointIndex, indexes.Y, y);
            data.setField<boost::uint32_t>(pointIndex, indexes.Z, z);
            data.setField<boost::uint16_t>(pointIndex, indexes.Intensity, intensity);
            data.setField<boost::uint8_t>(pointIndex, indexes.ReturnNumber, returnNum);
            data.setField<boost::uint8_t>(pointIndex, indexes.NumberOfReturns, numReturns);
            data.setField<boost::uint8_t>(pointIndex, indexes.ScanDirectionFlag, scanDirFlag);
            data.setField<boost::uint8_t>(pointIndex, indexes.EdgeOfFlightLine, flight);
            data.setField<boost::uint8_t>(pointIndex, indexes.Classification, classification);
            data.setField<boost::int8_t>(pointIndex, indexes.ScanAngleRank, scanAngleRank);
            data.setField<boost::uint8_t>(pointIndex, indexes.UserData, user);
            data.setField<boost::uint16_t>(pointIndex, indexes.PointSourceId, pointSourceId);
        }

        if (hasTime)
        {
            const double time = Utils::read_field<double>(p);
            data.setField<double>(pointIndex, indexes.Time, time);
        }

        if (hasColor)
        {
            const boost::uint16_t red = Utils::read_field<boost::uint16_t>(p);
            const boost::uint16_t green = Utils::read_field<boost::uint16_t>(p);
            const boost::uint16_t blue = Utils::read_field<boost::uint16_t>(p);

            data.setField<boost::uint16_t>(pointIndex, indexes.Red, red);
            data.setField<boost::uint16_t>(pointIndex, indexes.Green, green);
            data.setField<boost::uint16_t>(pointIndex, indexes.Blue, blue);       
        }
        
        data.setNumPoints(pointIndex+1);
    }

    delete[] buf;

    data.setSpatialBounds( lasHeader.getBounds() );

    return numPoints;
}

void Reader::addDefaultDimensions()
{
    Dimension x("X", dimension::SignedInteger, 4,  
                "X coordinate as a long integer. You must use the scale "
                "and offset information of the header to determine the double value." );
    x.setUUID("2ee118d1-119e-4906-99c3-42934203f872");
    addDefaultDimension(x, getName());

    Dimension y("Y", dimension::SignedInteger, 4,  
                "Y coordinate as a long integer. You must use the scale "
                "and offset information of the header to determine the double value." );
    y.setUUID("87707eee-2f30-4979-9987-8ef747e30275");
    addDefaultDimension(y, getName());

    Dimension z("Z", dimension::SignedInteger, 4,  
                "x coordinate as a long integer. You must use the scale and "
                "offset information of the header to determine the double value." );
    z.setUUID("e74b5e41-95e6-4cf2-86ad-e3f5a996da5d");
    addDefaultDimension(z, getName());

    Dimension time("Time", dimension::Float, 8, 
                    "The GPS Time is the double floating point time tag value at "
                    "which the point was acquired. It is GPS Week Time if the "
                    "Global Encoding low bit is clear and Adjusted Standard GPS "
                    "Time if the Global Encoding low bit is set (see Global Encoding "
                    "in the Public Header Block description)." );
    time.setUUID("aec43586-2711-4e59-9df0-65aca78a4ffc");
    addDefaultDimension(time, getName());

    Dimension intensity("Intensity", dimension::UnsignedInteger, 2,
                        "The intensity value is the integer representation of the pulse "
                        "return magnitude. This value is optional and system specific. "
                        "However, it should always be included if available." );
    intensity.setUUID("61e90c9a-42fc-46c7-acd3-20d67bd5626f");
    addDefaultDimension(intensity, getName());
    
    Dimension return_number("ReturnNumber", dimension::UnsignedInteger, 1,
                            "Return Number: The Return Number is the pulse return number for "
                            "a given output pulse. A given output laser pulse can have many "
                            "returns, and they must be marked in sequence of return. The first "
                            "return will have a Return Number of one, the second a Return "
                            "Number of two, and so on up to five returns." );
    return_number.setUUID("ffe5e5f8-4cec-4560-abf0-448008f7b89e");
    addDefaultDimension(return_number, getName());

    Dimension number_of_returns("NumberOfReturns", dimension::UnsignedInteger, 1, 
                                "Number of Returns (for this emitted pulse): The Number of Returns "
                                "is the total number of returns for a given pulse. For example, "
                                "a laser data point may be return two (Return Number) within a "
                                "total number of five returns." );
    number_of_returns.setUUID("7c28bfd4-a9ed-4fb2-b07f-931c076fbaf0");
    addDefaultDimension(number_of_returns, getName());
    
    Dimension scan_direction(   "ScanDirectionFlag", dimension::UnsignedInteger, 1,
                                "The Scan Direction Flag denotes the direction at which the "
                                "scanner mirror was traveling at the time of the output pulse. "
                                "A bit value of 1 is a positive scan direction, and a bit value "
                                "of 0 is a negative scan direction (where positive scan direction "
                                "is a scan moving from the left side of the in-track direction to "
                                "the right side and negative the opposite)." );
    scan_direction.setUUID("13019a2c-cf88-480d-a995-0162055fe5f9");
    addDefaultDimension(scan_direction, getName());

    Dimension edge( "EdgeOfFlightLine", dimension::UnsignedInteger, 1,
                    "The Edge of Flight Line data bit has a value of 1 only when "
                    "the point is at the end of a scan. It is the last point on "
                    "a given scan line before it changes direction." );
    edge.setUUID("108c18f2-5cc0-4669-ae9a-f41eb4006ea5");
    addDefaultDimension(edge, getName());
    
    Dimension classification(   "Classification", dimension::UnsignedInteger, 1,
                                "Classification in LAS 1.0 was essentially user defined and optional. "
                                "LAS 1.1 defines a standard set of ASPRS classifications. In addition, "
                                "the field is now mandatory. If a point has never been classified, this "
                                "byte must be set to zero. There are no user defined classes since "
                                "both point format 0 and point format 1 supply 8 bits per point for "
                                "user defined operations. Note that the format for classification is a "
                                "bit encoded field with the lower five bits used for class and the "
                                "three high bits used for flags." );
    classification.setUUID("b4c67de9-cef1-432c-8909-7c751b2a4e0b");
    addDefaultDimension(classification, getName());
    
    Dimension scan_angle(   "ScanAngleRank", dimension::SignedInteger, 1,
                            "The Scan Angle Rank is a signed one-byte number with a "
                            "valid range from -90 to +90. The Scan Angle Rank is the "
                            "angle (rounded to the nearest integer in the absolute "
                            "value sense) at which the laser point was output from the "
                            "laser system including the roll of the aircraft. The scan "
                            "angle is within 1 degree of accuracy from +90 to ñ90 degrees. "
                            "The scan angle is an angle based on 0 degrees being nadir, "
                            "and ñ90 degrees to the left side of the aircraft in the "
                            "direction of flight." );
    scan_angle.setUUID("aaadaf77-e0c9-4df0-81a7-27060794cd69");
    addDefaultDimension(scan_angle, getName());

    Dimension user_data("UserData", dimension::UnsignedInteger, 1,
                        "This field may be used at the users discretion");
    user_data.setUUID("70eb558e-63d4-4804-b1db-fc2fd716927c");
    addDefaultDimension(user_data, getName());

    Dimension point_source( "PointSourceId", dimension::UnsignedInteger, 2,
                            "This value indicates the file from which this point originated. "
                            "Valid values for this field are 1 to 65,535 inclusive with zero "
                            "being used for a special case discussed below. The numerical value "
                            "corresponds to the File Source ID from which this point originated. "
                            "Zero is reserved as a convenience to system implementers. A Point "
                            "Source ID of zero implies that this point originated in this file. "
                            "This implies that processing software should set the Point Source "
                            "ID equal to the File Source ID of the file containing this point "
                            "at some time during processing. " );
    point_source.setUUID("4e42e96a-6af0-4fdd-81cb-6216ff47bf6b");
    addDefaultDimension(point_source, getName());
    
    Dimension packet_descriptor("WavePacketDescriptorIndex", dimension::UnsignedInteger, 1);
    packet_descriptor.setUUID("1d095eb0-099f-4800-abb6-2272be486f81");
    addDefaultDimension(packet_descriptor, getName());
    
    Dimension packet_offset("WaveformDataOffset", dimension::UnsignedInteger, 8);
    packet_offset.setUUID("6dee8edf-0c2a-4554-b999-20c9d5f0e7b9");
    addDefaultDimension(packet_offset, getName());
    
    Dimension return_point("ReturnPointWaveformLocation", dimension::UnsignedInteger, 4);
    return_point.setUUID("f0f37962-2563-4c3e-858d-28ec15a1103f");
    addDefaultDimension(return_point, getName());
    
    Dimension wave_x("WaveformXt", dimension::Float, 4);
    wave_x.setUUID("c0ec76eb-9121-4127-b3d7-af92ef871a2d");
    addDefaultDimension(wave_x, getName());

    Dimension wave_y("WaveformYt", dimension::Float, 4);
    wave_y.setUUID("b3f5bb56-3c25-42eb-9476-186bb6b78e3c");
    addDefaultDimension(wave_y, getName());

    Dimension wave_z("WaveformZt", dimension::Float, 4);
    wave_z.setUUID("7499ae66-462f-4d0b-a449-6e5c721fb087");
    addDefaultDimension(wave_z, getName());
    
    Dimension red(  "Red", dimension::UnsignedInteger, 2,
                    "The red image channel value associated with this point");
    red.setUUID("a42ce297-6aa2-4a62-bd29-2db19ba862d5");
    addDefaultDimension(red, getName());

    Dimension blue(  "Blue", dimension::UnsignedInteger, 2,
                    "The blue image channel value associated with this point");
    blue.setUUID("5c1a99c8-1829-4d5b-8735-4f6f393a7970");
    addDefaultDimension(blue, getName());

    Dimension green(  "Green", dimension::UnsignedInteger, 2,
                    "The green image channel value associated with this point");
    green.setUUID("7752759d-5713-48cd-9842-51db350cc979");
    addDefaultDimension(green, getName());

}

boost::property_tree::ptree Reader::toPTree() const
{
    boost::property_tree::ptree tree = pdal::Reader::toPTree();

    tree.add("Compression", this->isCompressed());
    // add more stuff here specific to this stage type

    return tree;
}

} } } // namespaces
