import AudioToolbox
import XCTest
import RetroPlugKit

final class RetroPlugAudioUnitTests: XCTestCase {
    private func makeUnit() throws -> RetroPlugAudioUnit {
        let description = AudioComponentDescription(componentType: kAudioUnitType_MusicDevice,
                                                    componentSubType: 0x6d676273,
                                                    componentManufacturer: 0x5250746d,
                                                    componentFlags: 0,
                                                    componentFlagsMask: 0)
        return try RetroPlugAudioUnit(componentDescription: description, options: [])
    }

    func testLifecycleStateAndBusLayout() throws {
        let unit = try makeUnit()
        XCTAssertEqual(unit.outputBusses.count, 5)

        let state = try XCTUnwrap(unit.fullState)
        unit.fullState = state

        try unit.allocateRenderResources()
        unit.deallocateRenderResources()
    }
}
