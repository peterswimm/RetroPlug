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
        XCTAssertEqual(unit.parameterTree?.allParameters.count, 57)
        XCTAssertNotNil(unit.parameterTree?.parameter(withAddress: 16)) // first of 17 MIDI channels
        XCTAssertNotNil(unit.parameterTree?.parameter(withAddress: 48)) // first MI.OUT CC number
        XCTAssertNotNil(unit.parameterTree?.parameter(withAddress: 80)) // mGB base channel

        unit.setMidiChannel(9, for: .mgbPu1)
        unit.setMgbBaseChannel(7)
        unit.setMidiOutCcMode(.single, forVoice: 2)
        unit.setMidiOutCcScaling(false, forVoice: 2)
        unit.setMidiOutCcNumber(74, at: 3, forVoice: 2)
        XCTAssertEqual(unit.parameterTree?.parameter(withAddress: 20)?.value, 9)
        XCTAssertEqual(unit.parameterTree?.parameter(withAddress: 80)?.value, 7)
        XCTAssertEqual(unit.parameterTree?.parameter(withAddress: 42)?.value, 0)
        XCTAssertEqual(unit.parameterTree?.parameter(withAddress: 46)?.value, 0)
        XCTAssertEqual(unit.parameterTree?.parameter(withAddress: 65)?.value, 74)

        let state = try XCTUnwrap(unit.fullState)
        unit.fullState = state

        try unit.allocateRenderResources()
        unit.deallocateRenderResources()
    }
}
