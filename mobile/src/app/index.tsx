import { useState } from 'react';
import {
  ActivityIndicator,
  Pressable,
  ScrollView,
  StyleSheet,
  View,
} from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';
import * as DocumentPicker from 'expo-document-picker';
import { File } from 'expo-file-system';

import { ThemedText } from '@/components/themed-text';
import { ThemedView } from '@/components/themed-view';
import { parseTelemetryCsv, type TelemetryCsvRow } from '@/csv';
import { BottomTabInset, MaxContentWidth, Spacing } from '@/constants/theme';
import { useTheme } from '@/hooks/use-theme';

type ImportState =
  | { status: 'idle' }
  | { status: 'loading' }
  | { status: 'success'; fileName: string; row: TelemetryCsvRow }
  | { status: 'error'; message: string };

function MetricRow({ label, value }: { label: string; value: string }) {
  return (
    <View style={styles.metricRow}>
      <ThemedText themeColor="textSecondary">{label}</ThemedText>
      <ThemedText type="smallBold">{value}</ThemedText>
    </View>
  );
}

export default function HomeScreen() {
  const theme = useTheme();
  const [importState, setImportState] = useState<ImportState>({ status: 'idle' });

  async function handleImportCsv() {
    try {
      const picked = await DocumentPicker.getDocumentAsync({
        type: ['text/csv', 'text/comma-separated-values', 'text/plain', '*/*'],
        copyToCacheDirectory: true,
        multiple: false,
      });

      if (picked.canceled || !picked.assets?.[0]) {
        return;
      }

      setImportState({ status: 'loading' });

      const asset = picked.assets[0];
      const csvText = await new File(asset.uri).text();
      const parsed = parseTelemetryCsv(csvText);

      if (!parsed.ok) {
        setImportState({ status: 'error', message: parsed.error });
        return;
      }

      setImportState({
        status: 'success',
        fileName: asset.name,
        row: parsed.row,
      });
    } catch (error) {
      const message =
        error instanceof Error ? error.message : 'Could not read the selected file.';
      setImportState({ status: 'error', message });
    }
  }

  return (
    <ThemedView style={styles.container}>
      <SafeAreaView style={styles.safeArea} edges={['top', 'left', 'right']}>
        <ScrollView
          contentContainerStyle={styles.scrollContent}
          showsVerticalScrollIndicator={false}>
          <ThemedText type="subtitle">DragonSense</ThemedText>
          <ThemedText themeColor="textSecondary">
            Import a PERG telemetry CSV to preview the final workout row.
          </ThemedText>

          <Pressable
            accessibilityRole="button"
            disabled={importState.status === 'loading'}
            onPress={handleImportCsv}
            style={({ pressed }) => [
              styles.button,
              {
                backgroundColor: theme.backgroundSelected,
                opacity: pressed || importState.status === 'loading' ? 0.7 : 1,
              },
            ]}>
            {importState.status === 'loading' ? (
              <ActivityIndicator color={theme.text} />
            ) : (
              <ThemedText type="smallBold">Import CSV</ThemedText>
            )}
          </Pressable>

          {importState.status === 'error' ? (
            <ThemedView type="backgroundElement" style={styles.panel}>
              <ThemedText type="smallBold">Import failed</ThemedText>
              <ThemedText themeColor="textSecondary">{importState.message}</ThemedText>
            </ThemedView>
          ) : null}

          {importState.status === 'success' ? (
            <ThemedView type="backgroundElement" style={styles.panel}>
              <ThemedText type="smallBold">Final telemetry row</ThemedText>
              <ThemedText type="code" themeColor="textSecondary">
                {importState.fileName}
              </ThemedText>
              <MetricRow label="Elapsed" value={`${importState.row.elapsed_s} s`} />
              <MetricRow label="Distance" value={`${importState.row.distance_m} m`} />
              <MetricRow label="RPM" value={`${importState.row.rpm}`} />
              <MetricRow label="Pace" value={`${importState.row.pace_s} s / 500 m`} />
              <MetricRow label="Power" value={`${importState.row.watts} W`} />
              <MetricRow
                label="Stroke rate"
                value={`${importState.row.stroke_rate_spm} spm`}
              />
            </ThemedView>
          ) : null}
        </ScrollView>
      </SafeAreaView>
    </ThemedView>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
  },
  safeArea: {
    flex: 1,
    maxWidth: MaxContentWidth,
    width: '100%',
    alignSelf: 'center',
  },
  scrollContent: {
    paddingHorizontal: Spacing.four,
    paddingTop: Spacing.four,
    paddingBottom: BottomTabInset + Spacing.four,
    gap: Spacing.three,
  },
  button: {
    alignItems: 'center',
    justifyContent: 'center',
    minHeight: 48,
    borderRadius: Spacing.three,
    paddingHorizontal: Spacing.four,
    paddingVertical: Spacing.three,
  },
  panel: {
    gap: Spacing.two,
    paddingHorizontal: Spacing.three,
    paddingVertical: Spacing.three,
    borderRadius: Spacing.three,
  },
  metricRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    gap: Spacing.two,
  },
});
